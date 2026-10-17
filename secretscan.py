#!/usr/bin/env python3
## ProOS secret scanner
# Scans every file in the repo that .gitignore does not exclude (plus any file
# already tracked in .git/index, ignored or not) for hardcoded credentials,
# secrets and personal data. Matched values are redacted in the report.
#
# Usage: python3 secretscan.py [--root DIR] [--min-level high|medium|low|info]
#                              [--max-size MB] [--show-ignored]
# A line holding "secretscan:ignore" is skipped. Exit code 1 when anything at
# medium or above is found, 0 otherwise.

import argparse
import base64
import binascii
import math
import os
import re
import struct
import sys

## Settings

LEVELS = ["info", "low", "medium", "high"]
SCRIPT_NAME = "secretscan.py"
IGNORE_MARKER = "secretscan:ignore"
DEFAULT_MAX_MB = 50
SAMPLE_BYTES = 8192
MIN_STRING_RUN = 6

# Extensions whose unquoted right-hand sides are expressions, not literals
CODE_EXTENSIONS = {
    ".js", ".mjs", ".cjs", ".ts", ".py", ".c", ".cpp", ".cc", ".h", ".hpp",
    ".ino", ".java", ".go", ".rs", ".cs", ".php", ".rb", ".swift",
}

# A heredoc fed to a password tool, e.g. "smbpasswd -a -s user << EOD";
# group 1 is the terminator word
PASSWORD_HEREDOC = re.compile(r"\b[a-z]*passwd\b[^\n]*<<-?\s*['\"]?([A-Za-z_]+)['\"]?")
HEREDOC_RULE = ("password-heredoc", "medium", "Literal password in a heredoc fed to passwd / smbpasswd")

# Files whose name alone suggests a secret
SENSITIVE_NAMES = [
    (r"^id_(rsa|dsa|ecdsa|ed25519)$", "SSH private key file"),
    (r"\.(kdbx|kdb|p12|pfx|jks|keystore|keytab|ovpn|asc|gpg)$", "Key store / key file"),
    (r"^\.env(\..*)?$", "Environment file"),
    (r"^\.netrc$|^_netrc$", "netrc credentials"),
    (r"^\.htpasswd$", "htpasswd file"),
    (r"^(shadow|gshadow)(-)?$", "Password hash database"),
    (r"^rclone\.conf$", "rclone config (OAuth tokens)"),
    (r"^wpa_supplicant.*\.conf$|\.nmconnection$", "Wi-Fi network profile"),
    (r"^\.(bash|zsh|python|mysql|psql)_history$|^\.lesshst$", "Shell history"),
    (r"^(credentials|secrets?)(\..*)?$", "Credentials file"),
    (r"^known_hosts$", "SSH known_hosts (host inventory)"),
    (r"^smbpasswd$|^passdb\.tdb$|^secrets\.tdb$", "Samba password database"),
    (r"\.(unf|unifi|backup)$|^pfSense.*\.xml$|^config-.*\.xml$", "Network device config backup"),
    (r"^\.DS_Store$", "macOS folder metadata (lists file names)"),
]

# Name patterns whose findings are low rather than high
LOW_SENSITIVE_NAMES = ("known_hosts", "macOS folder metadata")

# Git-ignore patterns that exist to keep secrets out; a tracked file matching
# one of these is reported high, any other tracked-but-ignored file low
SECRET_IGNORE_HINTS = re.compile(
    r"creds|passw|authorized_keys|pia-wg|\.pem|\.crt|\.key|\.rsa|\.pub|"
    r"openapd|pve/network|\.env|secret|token", re.I)

# Domains in e-mail addresses that are not personal data
EMAIL_ALLOW = re.compile(
    r"@(example\.(com|org|net)|localhost|users\.noreply\.github\.com|"
    r"noreply\.|.*\.local$|.*\.home$|domain\.(com|tld)|email\.com|test\.com|"
    r"openssh\.com)$|"
    r"\.(service|socket|target|timer|mount|path|slice|device|swap)$", re.I)

# Public resolvers and documentation addresses that are not personal data
PUBLIC_IP_ALLOW = {"1.2.3.4", "8.8.8.8", "8.8.4.4", "1.1.1.1", "1.0.0.1", "9.9.9.9",
                   "149.112.112.112", "208.67.222.222", "208.67.220.220"}

# Values on the right of a secret-looking name that are not secrets
PLACEHOLDER_VALUE = re.compile(
    r"^(true|false|null|none|nil|undefined|yes|no|on|off|required|optional|"
    r"string|password|passwd|secret|token|changeme|xxx+|\*+|\.+|<.*>|\{.*\}|"
    r"\$.*|%.*%|%[-0-9.]*[a-z].*|\[.*\]|\(.*|.*\(.*|[0-9]{1,3}|"
    r"@.*|process\.env.*|os\.environ.*|getenv.*|"
    r"~?\.{0,2}/.*|[a-z][a-z0-9+]*:/.*)$", re.I)

## Detection rules
# (rule id, level, description, regex, group holding the secret value,
#  rule applies to binary files' string runs too)

RULES = [
    ("private-key", "high", "Private key block",
     r"-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY(?: BLOCK)?-----", 0, True),
    ("putty-key", "high", "PuTTY private key",
     r"PuTTY-User-Key-File-\d", 0, True),
    ("aws-key", "high", "AWS access key id",
     r"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b", 0, True),
    ("github-token", "high", "GitHub token",
     r"\b(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{60,})\b", 0, True),
    ("gitlab-token", "high", "GitLab token",
     r"\bglpat-[A-Za-z0-9_-]{20,}\b", 0, True),
    ("slack-token", "high", "Slack token / webhook",
     r"\bxox[abposr]-[A-Za-z0-9-]{10,}|hooks\.slack\.com/services/[A-Za-z0-9/]+", 0, True),
    ("discord-webhook", "high", "Discord webhook",
     r"discord(?:app)?\.com/api/webhooks/\d+/[A-Za-z0-9_-]+", 0, True),
    ("google-api-key", "high", "Google API key",
     r"\bAIza[0-9A-Za-z_-]{35}\b", 0, True),
    ("google-oauth", "high", "Google OAuth secret / token",
     r"\bGOCSPX-[A-Za-z0-9_-]{20,}|\bya29\.[A-Za-z0-9_-]{20,}|\b1//0[A-Za-z0-9_-]{30,}", 0, True),
    ("stripe-key", "high", "Stripe live key",
     r"\b[sr]k_live_[A-Za-z0-9]{20,}\b", 0, True),
    ("ai-api-key", "high", "AI provider API key",
     r"\bsk-(?:ant-|proj-)?[A-Za-z0-9_-]{32,}\b", 0, True),
    ("sendgrid-key", "high", "SendGrid key",
     r"\bSG\.[A-Za-z0-9_-]{16,}\.[A-Za-z0-9_-]{16,}\b", 0, True),
    ("telegram-token", "high", "Telegram bot token",
     r"\b\d{8,10}:AA[A-Za-z0-9_-]{33}\b", 0, True),
    ("jwt", "high", "JSON Web Token",
     r"\beyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}", 0, True),
    ("wireguard-key", "high", "WireGuard private / preshared key",
     r"(?i)\b(?:PrivateKey|PresharedKey)\s*=\s*([A-Za-z0-9+/]{42,43}=)", 1, True),
    ("url-credentials", "high", "Credentials inside a URL",
     r"\b[a-z][a-z0-9+.-]{1,15}://([^\s:/@'\"<>]{1,64}:[^\s@/'\"<>]{1,128})@[A-Za-z0-9.-]+", 1, True),
    ("auth-header", "high", "Literal Authorization header",
     r"(?i)\bAuthorization['\"]?\s*[:=]\s*['\"]?(?:Basic|Bearer|Token)\s+([A-Za-z0-9+/=._-]{12,})", 1, False),
    ("wifi-qr", "high", "Wi-Fi QR string with password",
     r"WIFI:[^\n]*?P:([^;\n]+);", 1, False),
    ("password-hash", "high", "Password hash (crypt / bcrypt / yescrypt)",
     r"(\$(?:1|2[abxy]|5|6|7|y|gy|sha1|apr1)\$[./A-Za-z0-9$]{8,}[./A-Za-z0-9]{20,})", 1, True),
    ("secret-assign", "medium", "Secret-named setting with a literal value",
     r"(?i)(?<![A-Za-z0-9])([A-Za-z0-9_.-]*?(?:password|passwd|passphrase|pass|pwd|secret|"
     r"token|api[_-]?key|apikey|auth[_-]?key|access[_-]?key|private[_-]?key|"
     r"psk|client[_-]?secret|credentials?|wpa[_-]?key)[A-Za-z0-9_.-]*)['\"]?\s*"
     r"(?::=|=>|=|:)\s*(['\"]?)([^\s'\"`;,)]{3,})\2", 3, False),
    ("cli-password", "medium", "Password passed on a command line",
     r"(?i)(?:sshpass\s+-p\s*|--pass(?:word)?[= ]+|\bmysql\b[^\n]*\s-p|"
     r"\bsmbclient\b[^\n]*%|\bcurl\b[^\n]*\s-u\s+[^\s:]+:)(['\"]?[^\s'\"$-][^\s'\"]{2,})", 1, False),
    ("chpasswd", "medium", "Password piped into chpasswd / passwd / smbpasswd",
     r"(?i)echo\s+(?:-[ne]+\s+)?['\"]?([^|$'\"]{3,})['\"]?\s*\|\s*(?:sudo\s+)?[a-z]*passwd\b", 1, False),
    ("high-entropy", "medium", "High-entropy string in a secret-like context",
     r"(?i)(?:key|secret|token|pass|pwd|auth|sig|salt|hash)[A-Za-z0-9_]*['\"]?\s*[:=]\s*['\"]([A-Za-z0-9+/=_-]{24,})['\"]", 1, False),
    ("base64-text", "medium", "Base64 literal that decodes to plain text (obfuscated secret?)",
     r"(['\"])([A-Za-z0-9+/_-]{12,}(?:={0,2}|@{0,2}))\1", 2, False),
    ("email", "low", "E-mail address",
     r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b", 0, False),
    ("phone", "low", "Phone number",
     r"(?<![\d.])(?:\+?1[ .-])?\(?[2-9]\d{2}\)?[ .-]\d{3}[ .-]\d{4}(?![\d.])", 0, False),
    ("street-address", "low", "Street address",
     r"\b\d{1,5}\s+(?:[A-Z][a-z]+\s+){1,3}(?:Street|St|Avenue|Ave|Road|Rd|Boulevard|Blvd|"
     r"Lane|Ln|Drive|Court|Ct|Place|Pl|Terrace|Parkway|Pkwy|Highway|Hwy)\b\.?", 0, False),
    ("coordinates", "low", "Geographic coordinates",
     r"(?i)(?<![A-Za-z])(?:lat(?:itude)?|lon(?:gitude)?|lng)['\"]?\s*[:=]\s*['\"]?(-?\d{1,3}\.\d{3,})", 1, False),
    ("public-ip", "info", "Public IP address",
     r"(?<![\d.])((?:\d{1,3}\.){3}\d{1,3})(?![\d.])", 1, False),
    ("mac-address", "info", "MAC address (device identifier)",
     r"(?<![0-9A-Fa-f:-])((?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2})(?![0-9A-Fa-f:-])", 1, False),
]

COMPILED_RULES = [
    (ruleId, level, description, re.compile(pattern), group, inBinary)
    for (ruleId, level, description, pattern, group, inBinary) in RULES
]


## Gitignore matching

def globToRegex(pattern):
    """Turn one gitignore glob into a regex string matched against a
    slash-separated relative path, e.g. "build/**/*.o" -> "build/(?:.*/)?[^/]*\\.o"."""
    regexParts = []
    index = 0
    length = len(pattern)
    while index < length:
        char = pattern[index]
        if pattern.startswith("**/", index):
            regexParts.append("(?:.*/)?")
            index = index + 3
        elif pattern.startswith("/**", index) and index + 3 == length:
            regexParts.append("/.*")
            index = index + 3
        elif pattern.startswith("**", index):
            regexParts.append(".*")
            index = index + 2
        elif char == "*":
            regexParts.append("[^/]*")
            index = index + 1
        elif char == "?":
            regexParts.append("[^/]")
            index = index + 1
        elif char == "[":
            closing = pattern.find("]", index + 1)
            if closing == -1:
                regexParts.append(re.escape(char))
                index = index + 1
            else:
                charClass = pattern[index + 1:closing]
                if charClass.startswith("!"):
                    charClass = "^" + charClass[1:]
                regexParts.append("[" + charClass + "]")
                index = closing + 1
        elif char == "\\" and index + 1 < length:
            regexParts.append(re.escape(pattern[index + 1]))
            index = index + 2
        else:
            regexParts.append(re.escape(char))
            index = index + 1
    return "".join(regexParts)


def loadIgnoreFile(filePath, baseDir):
    """Read one .gitignore into rule dicts relative to baseDir ("" for the
    repo root), e.g. "*.rsa" -> {regex, negate, dirOnly, anchored, base}."""
    rules = []
    try:
        with open(filePath, "r", encoding="utf-8", errors="replace") as handle:
            lines = handle.read().splitlines()
    except OSError:
        return rules
    for rawLine in lines:
        line = rawLine.rstrip()
        if line == "" or line.startswith("#"):
            continue
        negate = False
        if line.startswith("!"):
            negate = True
            line = line[1:]
        if line.startswith("\\"):
            line = line[1:]
        dirOnly = False
        if line.endswith("/"):
            dirOnly = True
            line = line.rstrip("/")
        anchored = "/" in line
        line = line.lstrip("/")
        rules.append({
            "regex": re.compile("^" + globToRegex(line) + "$"),
            "negate": negate,
            "dirOnly": dirOnly,
            "anchored": anchored,
            "base": baseDir,
            "source": filePath,
        })
    return rules


def isIgnored(relPath, isDir, ignoreRules):
    """Return the matching rule when relPath (repo-relative, slash-separated)
    is ignored by the rules in force, else None. Last match wins."""
    matched = None
    for rule in ignoreRules:
        base = rule["base"]
        if base != "":
            if not relPath.startswith(base + "/"):
                continue
            localPath = relPath[len(base) + 1:]
        else:
            localPath = relPath
        if rule["dirOnly"] and not isDir:
            continue
        if rule["anchored"]:
            target = localPath
        else:
            target = localPath.rsplit("/", 1)[-1]
        if rule["regex"].match(target):
            if rule["negate"]:
                matched = None
            else:
                matched = rule
    return matched


## Tracked files

def readIndexPaths(repoRoot):
    """Return the set of paths tracked in .git/index (versions 2 and 3),
    e.g. {"login.sh", "rpi/rpi.sh"}; an empty set when there is no index."""
    indexPath = os.path.join(repoRoot, ".git", "index")
    tracked = set()
    try:
        with open(indexPath, "rb") as handle:
            data = handle.read()
    except OSError:
        return tracked
    if data[:4] != b"DIRC":
        return tracked
    version, entryCount = struct.unpack(">II", data[4:12])
    if version not in (2, 3):
        print("warning: index version %d not supported, tracked-file check skipped" % version,
              file=sys.stderr)
        return tracked
    offset = 12
    for _ in range(entryCount):
        entryStart = offset
        flags = struct.unpack(">H", data[offset + 60:offset + 62])[0]
        pathStart = offset + 62
        if version == 3 and (flags & 0x4000):
            pathStart = pathStart + 2
        pathEnd = data.index(b"\x00", pathStart)
        tracked.add(data[pathStart:pathEnd].decode("utf-8", errors="replace"))
        entryLength = pathEnd - entryStart
        paddedLength = (entryLength // 8 + 1) * 8
        offset = entryStart + paddedLength
    return tracked


## File walk

def collectFiles(repoRoot, showIgnored):
    """Walk the repo and return (filesToScan, ignoredTracked, ignoredList):
    every non-ignored file, tracked files that .gitignore excludes, and (with
    showIgnored) the ignored paths with the rule that excluded them."""
    ignoreRules = []
    excludePath = os.path.join(repoRoot, ".git", "info", "exclude")
    ignoreRules.extend(loadIgnoreFile(excludePath, ""))
    filesToScan = []
    ignoredList = []

    for dirPath, dirNames, fileNames in os.walk(repoRoot):
        relDir = os.path.relpath(dirPath, repoRoot).replace(os.sep, "/")
        if relDir == ".":
            relDir = ""
        if ".gitignore" in fileNames:
            ignoreRules.extend(loadIgnoreFile(os.path.join(dirPath, ".gitignore"), relDir))
        keptDirs = []
        for dirName in sorted(dirNames):
            relChild = dirName if relDir == "" else relDir + "/" + dirName
            if relChild == ".git":
                continue
            rule = isIgnored(relChild, True, ignoreRules)
            if rule is None:
                keptDirs.append(dirName)
            elif showIgnored:
                ignoredList.append((relChild + "/", rule))
        dirNames[:] = keptDirs
        for fileName in sorted(fileNames):
            relFile = fileName if relDir == "" else relDir + "/" + fileName
            fullPath = os.path.join(dirPath, fileName)
            if os.path.islink(fullPath) or not os.path.isfile(fullPath):
                continue
            rule = isIgnored(relFile, False, ignoreRules)
            if rule is None:
                filesToScan.append(relFile)
            elif showIgnored:
                ignoredList.append((relFile, rule))

    tracked = readIndexPaths(repoRoot)
    scanSet = set(filesToScan)
    ignoredTracked = []
    for trackedPath in sorted(tracked):
        if trackedPath in scanSet:
            continue
        fullPath = os.path.join(repoRoot, trackedPath)
        if os.path.isfile(fullPath):
            rule = ignoreRuleFor(trackedPath, ignoreRules)
            pattern = rule["regex"].pattern if rule else "?"
            ignoredTracked.append((trackedPath, pattern))
            filesToScan.append(trackedPath)
    return filesToScan, ignoredTracked, ignoredList


def ignoreRuleFor(relPath, ignoreRules):
    """The rule that ignores relPath, checking each parent folder first, e.g.
    "pve/network/a.unf" -> the "pve/network" rule; None when it is not ignored."""
    parts = relPath.split("/")
    for depth in range(1, len(parts)):
        parentPath = "/".join(parts[:depth])
        rule = isIgnored(parentPath, True, ignoreRules)
        if rule is not None:
            return rule
    return isIgnored(relPath, False, ignoreRules)


## Scanning helpers

def redact(value):
    """Mask a matched value for the report: keep its first two characters and
    its length, e.g. "hunter2pass" -> "hu*********(11)". E-mail addresses keep
    their domain: "ben@gmail.com" -> "b**@gmail.com"."""
    if "@" in value and re.match(r"^[^@\s]+@[^@\s]+$", value):
        localPart, domain = value.split("@", 1)
        maskedLocal = localPart[:1] + "*" * max(len(localPart) - 1, 2)
        return maskedLocal + "@" + domain
    if len(value) <= 4:
        return "*" * len(value)
    shown = value[:2]
    hiddenCount = len(value) - 2
    return shown + "*" * min(hiddenCount, 12) + "(" + str(len(value)) + ")"


def shannonEntropy(text):
    """Bits of entropy per character, e.g. "aaaa" -> 0.0, a random base64 key ~ 5.5."""
    if text == "":
        return 0.0
    counts = {}
    for char in text:
        counts[char] = counts.get(char, 0) + 1
    total = len(text)
    entropy = 0.0
    for count in counts.values():
        probability = count / total
        entropy = entropy - probability * math.log2(probability)
    return entropy


def isPrivateIp(address):
    """True for addresses that are not on the public internet (RFC 1918,
    loopback, link-local, CGNAT, multicast, reserved) or are not valid IPs."""
    try:
        octets = [int(part) for part in address.split(".")]
    except ValueError:
        return True
    if len(octets) != 4 or any(octet > 255 for octet in octets):
        return True
    first = octets[0]
    second = octets[1]
    if first in (0, 10, 127) or first >= 224:
        return True
    if first == 172 and 16 <= second <= 31:
        return True
    if first == 192 and second == 168:
        return True
    if first == 169 and second == 254:
        return True
    if first == 100 and 64 <= second <= 127:
        return True
    if first == 192 and second == 0:
        return True
    if first == 198 and second in (18, 19, 51):
        return True
    if first == 203 and second == 0:
        return True
    return False


def looksLikeVersion(line, start, end):
    """True when a dotted number sits in a version / OID context, e.g.
    "version 1.2.3.4" or "1.3.6.1.4.1", so it is not reported as an IP."""
    before = line[max(0, start - 12):start].lower()
    after = line[end:end + 1]
    if after == "." or line[start - 1:start] in (".", "v", "V", "/"):
        return True
    return any(word in before for word in ("version", "ver ", "v=", "oid", "build"))


def secretAssignIsReal(match, extension):
    """Filter for the secret-assign rule: drop placeholders, variable
    references and, in code files, unquoted expressions like pwd = input.value."""
    nameText = match.group(1)
    quote = match.group(2)
    value = match.group(3)
    if PLACEHOLDER_VALUE.match(value):
        return False
    if value.lower() == nameText.lower():
        return False
    if extension in CODE_EXTENSIONS and quote == "":
        return False
    lowerName = nameText.lower()
    if lowerName == "nopasswd":
        return False
    separator = match.group(0)[len(nameText):].lstrip("'\" \t")[:1]
    if separator == ":" and quote == "" and isCommentLine(match.string):
        return False
    # "pass" alone is too common (bypass, compass, passthrough): require it to
    # stand as its own word part
    if "pass" in lowerName and not re.search(
            r"(password|passwd|passphrase|pwd|secret|token|key|psk|cred)", lowerName):
        if not re.search(r"(^|[_.-])pass($|[_.-])", lowerName):
            return False
    if re.search(r"(file|path|dir|url|len|length|size|count|type|name|id|"
                 r"field|label|input|prompt|regex|pattern|hash|fn|cb|btn|button|"
                 r"element|class|style|timeout|window|menu|enabled|lock|rounds|"
                 r"iterations|_?ms)$", lowerName):
        return False
    return True


def isCommentLine(lineText):
    """True when a line is a comment in shell, C/JS or INI style, e.g. "## Boot" or "// note"."""
    stripped = lineText.lstrip()
    return stripped.startswith(("#", "//", "/*", "*", ";", "--"))


def decodesToText(literal):
    """True when a base64 literal (standard, URL-safe, or the site's '@' for '='
    padding) decodes to 8+ bytes of printable ASCII, e.g. "aHVudGVyMjpzZWNyZXQ=" -> True.
    Words and identifiers decode to binary noise, so they return False."""
    normalised = literal.replace("@", "=").replace("-", "+").replace("_", "/")
    if len(normalised) % 4 != 0:
        return False
    if re.match(r"^[A-Za-z]+$", normalised) or re.match(r"^[0-9]+$", normalised):
        return False
    try:
        decoded = base64.b64decode(normalised, validate=True)
    except (binascii.Error, ValueError):
        return False
    if len(decoded) < 8:
        return False
    return all(0x20 <= byte <= 0x7e for byte in decoded)


def buildFinding(relPath, lineNumber, rule, valueText, lineText):
    """One finding dict for the report, with the secret value redacted
    inside the shown line, e.g. {"path", "line", "rule", "level", "snippet"}."""
    ruleId, level, description = rule[0], rule[1], rule[2]
    shownLine = lineText.strip()
    if valueText:
        shownLine = shownLine.replace(valueText, redact(valueText))
    if len(shownLine) > 160:
        shownLine = shownLine[:157] + "..."
    return {
        "path": relPath,
        "line": lineNumber,
        "rule": ruleId,
        "level": level,
        "description": description,
        "snippet": shownLine,
    }


def scanLine(relPath, lineNumber, lineText, extension, binaryMode):
    """Apply every rule to one line (or one printable string run of a binary)
    and return its findings."""
    findings = []
    if IGNORE_MARKER in lineText:
        return findings
    for rule in COMPILED_RULES:
        ruleId, level, description, regex, group, inBinary = rule
        if binaryMode and not inBinary:
            continue
        for match in regex.finditer(lineText):
            valueText = match.group(group)
            if ruleId == "secret-assign" and not secretAssignIsReal(match, extension):
                continue
            if ruleId == "base64-text" and not decodesToText(valueText):
                continue
            if ruleId == "high-entropy":
                if re.match(r"^[0-9a-fA-F]+$", valueText):
                    continue
                if shannonEntropy(valueText) < 4.0:
                    continue
            if ruleId == "email" and EMAIL_ALLOW.search(valueText):
                continue
            if ruleId == "email" and re.search(r"\.(png|jpe?g|gif|svg|js|css|webp)$", valueText, re.I):
                continue
            if ruleId == "public-ip":
                if isPrivateIp(valueText) or valueText in PUBLIC_IP_ALLOW:
                    continue
                if looksLikeVersion(lineText, match.start(group), match.end(group)):
                    continue
            if ruleId == "mac-address" and valueText.replace(":", "").replace("-", "") in (
                    "000000000000", "ffffffffffff", "FFFFFFFFFFFF"):
                continue
            if ruleId == "url-credentials" and PLACEHOLDER_VALUE.match(valueText.split(":", 1)[1]):
                continue
            findings.append(buildFinding(relPath, lineNumber, rule, valueText, lineText))
    return findings


def isBinary(sample):
    """True when the first bytes of a file hold a NUL byte."""
    return b"\x00" in sample


def binaryStringRuns(data):
    """Yield (offset, text) for every printable ASCII run of at least
    MIN_STRING_RUN bytes in a binary, like the strings(1) tool."""
    pattern = re.compile(rb"[\x20-\x7e]{%d,}" % MIN_STRING_RUN)
    for match in pattern.finditer(data):
        yield match.start(), match.group().decode("ascii")


def scanFile(repoRoot, relPath, maxBytes):
    """Scan one file and return (findings, skippedReason or None)."""
    fullPath = os.path.join(repoRoot, relPath)
    findings = []
    baseName = relPath.rsplit("/", 1)[-1]
    for namePattern, description in SENSITIVE_NAMES:
        if re.search(namePattern, baseName, re.I):
            findings.append({
                "path": relPath, "line": 0, "rule": "sensitive-filename",
                "level": "low" if any(word in description for word in LOW_SENSITIVE_NAMES) else "high",
                "description": description, "snippet": "(file name)",
            })
    try:
        fileSize = os.path.getsize(fullPath)
    except OSError as error:
        return findings, "unreadable: %s" % error
    if fileSize > maxBytes:
        return findings, "larger than limit (%d MB)" % (fileSize // (1024 * 1024))
    try:
        with open(fullPath, "rb") as handle:
            data = handle.read()
    except OSError as error:
        return findings, "unreadable: %s" % error
    extension = os.path.splitext(baseName)[1].lower()
    if isBinary(data[:SAMPLE_BYTES]):
        for offset, text in binaryStringRuns(data):
            for finding in scanLine(relPath, offset, text, extension, True):
                finding["line"] = "@0x%x" % offset
                findings.append(finding)
        return findings, None
    text = data.decode("utf-8", errors="replace")
    heredocTerminator = None
    for lineIndex, lineText in enumerate(text.splitlines()):
        lineNumber = lineIndex + 1
        if len(lineText) > 4000:
            lineText = lineText[:4000]
        if heredocTerminator is not None:
            if lineText.strip() == heredocTerminator:
                heredocTerminator = None
            elif "$" not in lineText and lineText.strip() != "" and IGNORE_MARKER not in lineText:
                findings.append(buildFinding(relPath, lineNumber, HEREDOC_RULE, lineText.strip(), lineText))
            continue
        heredocMatch = PASSWORD_HEREDOC.search(lineText)
        if heredocMatch and not isCommentLine(lineText):
            heredocTerminator = heredocMatch.group(1)
        findings.extend(scanLine(relPath, lineNumber, lineText, extension, False))
    return findings, None


## Report

def collapseRepeats(findings):
    """Merge findings of one rule in one file that show the same redacted
    snippet text after line numbers are dropped, keeping the first line and a
    repeat count, e.g. 12 identical EAGLE author lines -> one with repeats 12."""
    collapsed = []
    seen = {}
    for finding in findings:
        key = (finding["path"], finding["rule"], finding["snippet"])
        if finding["rule"] == "email":
            emailMatch = re.search(r"[^\s<>;&()]*\*+@[A-Za-z0-9.-]+", finding["snippet"])
            if emailMatch:
                key = (finding["path"], finding["rule"], emailMatch.group())
        if key in seen:
            seen[key]["repeats"] = seen[key]["repeats"] + 1
            continue
        entry = dict(finding)
        entry["repeats"] = 1
        seen[key] = entry
        collapsed.append(entry)
    return collapsed


def printReport(findings, ignoredTracked, skipped, scannedCount, minLevel, ignoredList):
    """Print findings grouped by level, then file, highest level first."""
    minRank = LEVELS.index(minLevel)
    print("ProOS secret scan: %d files scanned (not git-ignored, or tracked)" % scannedCount)
    print()
    if ignoredTracked:
        print("== TRACKED BUT GIT-IGNORED (in .git/index, so .gitignore does not keep them out of commits)")
        for path, pattern in ignoredTracked:
            level = "high" if SECRET_IGNORE_HINTS.search(pattern) else "low"
            print("   %-5s %s  [ignored by %s]" % (level, path, pattern))
        print()
    for level in reversed(LEVELS):
        if LEVELS.index(level) < minRank:
            continue
        levelFindings = [finding for finding in findings if finding["level"] == level]
        if not levelFindings:
            continue
        print("== %s (%d)" % (level.upper(), len(levelFindings)))
        currentPath = None
        for finding in collapseRepeats(levelFindings):
            if finding["path"] != currentPath:
                currentPath = finding["path"]
                print("  " + currentPath)
            repeatNote = ""
            if finding["repeats"] > 1:
                repeatNote = "  (+%d more like it)" % (finding["repeats"] - 1)
            print("    %-6s %-18s %s%s" % (finding["line"], finding["rule"], finding["snippet"], repeatNote))
        print()
    if skipped:
        print("== SKIPPED (%d)" % len(skipped))
        for path, reason in skipped:
            print("   %s  [%s]" % (path, reason))
        print()
    if ignoredList:
        print("== GIT-IGNORED, NOT SCANNED (%d)" % len(ignoredList))
        for path, rule in ignoredList:
            print("   %s  [%s]" % (path, rule["regex"].pattern))
        print()
    print("== SUMMARY")
    for level in reversed(LEVELS):
        count = sum(1 for finding in findings if finding["level"] == level)
        fileCount = len({finding["path"] for finding in findings if finding["level"] == level})
        print("   %-6s %4d findings in %d files" % (level, count, fileCount))


## Main

def main():
    """Parse arguments, collect and scan the files, print the report."""
    defaultRoot = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description="Scan ProOS for hardcoded secrets and PII")
    parser.add_argument("--root", default=defaultRoot, help="repo root (default: this script's folder)")
    parser.add_argument("--min-level", default="info", choices=LEVELS, help="lowest level to print")
    parser.add_argument("--max-size", type=int, default=DEFAULT_MAX_MB, help="skip files over this many MB")
    parser.add_argument("--show-ignored", action="store_true", help="list the git-ignored paths too")
    arguments = parser.parse_args()

    repoRoot = os.path.abspath(arguments.root)
    maxBytes = arguments.max_size * 1024 * 1024
    filesToScan, ignoredTracked, ignoredList = collectFiles(repoRoot, arguments.show_ignored)

    allFindings = []
    skipped = []
    for relPath in sorted(filesToScan):
        if relPath == SCRIPT_NAME:
            continue
        findings, skipReason = scanFile(repoRoot, relPath, maxBytes)
        allFindings.extend(findings)
        if skipReason:
            skipped.append((relPath, skipReason))

    levelRank = {level: LEVELS.index(level) for level in LEVELS}
    allFindings.sort(key=lambda finding: (-levelRank[finding["level"]], finding["path"],
                                          str(finding["line"]).zfill(8)))
    printReport(allFindings, ignoredTracked, skipped, len(filesToScan), arguments.min_level, ignoredList)

    seriousCount = sum(1 for finding in allFindings if levelRank[finding["level"]] >= levelRank["medium"])
    return 1 if seriousCount > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
