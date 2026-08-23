const { runFaviconLookup } = require('./favicon');

// The lookup answers with both fields; icon is "" when none was found.
// A failure answers an empty title rather than an error string: the website
// reports it with a glyph on the lookup button, so a message here would only
// get written into the name box as though it were the site's real title.
const errorResult = () => ({ title: "", icon: "" });

async function runSiteLookup(subCmd) {
    try {
        // Decode the base64 URL
        let base64In = subCmd.replace(/-/g, "+").replace(/_/g, "/").replace(/@/g, "=");
        let decodedUrl = Buffer.from(base64In, 'base64').toString('utf8');
        let urlObj = new URL(decodedUrl);

        // Handle hardcoded special cases
        // (the page is not fetched for these, so the favicon lookup falls
        // through to /favicon.ico on the same origin)
        if (urlObj.hostname === "www.ups.com" || urlObj.hostname === "ups.com") {
            return { title: "UPS Tracking", icon: await runFaviconLookup(decodedUrl, '') };
        }
        if (urlObj.hostname === "tools.usps.com") {
            return { title: "USPS Tracking", icon: await runFaviconLookup(decodedUrl, '') };
        }

        // Fetch the HTML natively (Requires Node.js 18+)
        const response = await fetch(decodedUrl, {
            method: 'GET',
            // Adding a standard user-agent helps prevent sites from blocking the request
            headers: {
                'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/114.0.0.0 Safari/537.36'
            }
        });

        if (!response.ok) {
            return errorResult();
        }

        const html = await response.text();

        // Pull the favicon out of the same markup: 16x16, 32 colours, JPEG,
        // URL-safe base64. Never throws, answers "" when it cannot build one.
        const icon = await runFaviconLookup(decodedUrl, html);

        // Extract the title using Regex (matches <title> optionally with attributes, and captures the content)
        // The 'i' flag makes it case-insensitive, the 's' flag allows '.' to match newlines
        const titleMatch = html.match(/<title[^>]*>(.*?)<\/title>/is);

        if (titleMatch && titleMatch[1]) {
            let title = titleMatch[1].trim();

            // Clean up common HTML entities that might be present in the title
            title = title.replace(/&amp;/g, '&')
                            .replace(/&lt;/g, '<')
                            .replace(/&gt;/g, '>')
                            .replace(/&quot;/g, '"')
                            .replace(/&#39;/g, "'");

            if (title === "") {
                return errorResult();
            }

            // Remove any accidental newlines or extra spaces parsed from the HTML
            return { title: title.replace(/\s+/g, ' '), icon: icon };
        }
        return errorResult();
    } catch (e) {
        // This catches invalid URLs, network timeouts, and fetch errors
        return errorResult();
    }
}

module.exports = { runSiteLookup };
