const ERR_MSG = "Site Lookup Error!";

async function runSiteLookup(subCmd) {
    try {
        // Decode the base64 URL
        let base64In = subCmd.replace(/-/g, "+").replace(/_/g, "/").replace(/@/g, "=");
        let decodedUrl = Buffer.from(base64In, 'base64').toString('utf8');
        let urlObj = new URL(decodedUrl);

        // Handle hardcoded special cases
        if (urlObj.hostname === "www.ups.com" || urlObj.hostname === "ups.com") {
            return "UPS Tracking";
        }
        if (urlObj.hostname === "tools.usps.com") {
            return "USPS Tracking";
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
            return ERR_MSG;
        }

        const html = await response.text();

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
                return ERR_MSG;
            }
            
            // Remove any accidental newlines or extra spaces parsed from the HTML
            return title.replace(/\s+/g, ' '); 
        }
        return ERR_MSG;
    } catch (e) {
        // This catches invalid URLs, network timeouts, and fetch errors
        return ERR_MSG;
    }
}

module.exports = { runSiteLookup };
