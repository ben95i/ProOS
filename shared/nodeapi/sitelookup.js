const { runFaviconLookup } = require('./favicon');

const FETCH_HEADERS = {
    'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 ' +
        '(KHTML, like Gecko) Chrome/114.0.0.0 Safari/537.36'
};

// Sites whose own markup gives no usable title. The page is not fetched for
// these, so the favicon lookup falls through to /favicon.ico on the origin.
const FIXED_TITLES = {
    "www.ups.com": "UPS Tracking",
    "ups.com": "UPS Tracking",
    "tools.usps.com": "USPS Tracking"
};

// A failure answers an empty title rather than an error string: the website
// reports it with a glyph on the lookup button, so a message here would only
// get written into the name box as though it were the site's real title.
const errorResult = () => ({ title: "", icon: "" });

// the site's <title>, with the common HTML entities turned back
function extractTitle(html) {
    const titleMatch = html.match(/<title[^>]*>(.*?)<\/title>/is);
    if (!titleMatch || !titleMatch[1]) {
        return "";
    }
    const title = titleMatch[1].trim()
        .replace(/&amp;/g, '&')
        .replace(/&lt;/g, '<')
        .replace(/&gt;/g, '>')
        .replace(/&quot;/g, '"')
        .replace(/&#39;/g, "'");
    if (title === "") {
        return "";
    }
    // drop any newlines and runs of spaces parsed out of the markup
    return title.replace(/\s+/g, ' ');
}

/**
 * Fetches a page and answers { title, icon } for it. subCmd is the URL in
 * the site's URL-safe base64. Never throws: an invalid URL, a network
 * failure or a page with no title all answer { title: "", icon: "" }.
 */
async function runSiteLookup(subCmd) {
    try {
        const base64In = subCmd.replace(/-/g, "+").replace(/_/g, "/").replace(/@/g, "=");
        const decodedUrl = Buffer.from(base64In, 'base64').toString('utf8');
        const urlObj = new URL(decodedUrl);
        const fixedTitle = FIXED_TITLES[urlObj.hostname];
        if (fixedTitle) {
            return { title: fixedTitle, icon: await runFaviconLookup(decodedUrl, '') };
        }
        const response = await fetch(decodedUrl, {
            method: 'GET',
            // a standard user-agent stops sites blocking the request outright
            headers: FETCH_HEADERS
        });
        if (!response.ok) {
            return errorResult();
        }
        const html = await response.text();
        // pulled out of the same markup: 24x24, 32 colours, URL-safe base64
        // PNG. Never throws, answers "" when it cannot build one.
        const icon = await runFaviconLookup(decodedUrl, html);
        const title = extractTitle(html);
        if (title === "") {
            return errorResult();
        }
        return { title: title, icon: icon };
    } catch (e) {
        // invalid URLs, network timeouts and fetch errors all land here
        return errorResult();
    }
}

module.exports = { runSiteLookup };
