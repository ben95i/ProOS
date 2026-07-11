const fsPromises = require('fs/promises');
const path = require('path');

const WEATHER_REFRESH = 5 * 60 * 1000; // 5 minutes

let weatherConfig = null;
let weatherCache = null;
let lastWeatherFetch = 0;

async function loadWeatherConfig() {
    if (weatherConfig) return weatherConfig;
    try {
        const configPath = path.join(__dirname, 'weather.json');
        const fileData = await fsPromises.readFile(configPath, 'utf8');
        weatherConfig = JSON.parse(fileData);
        return weatherConfig;
    } catch (error) {
        console.error("Failed to load weather config:", error.message);
        throw error;
    }
}

function decodeWeatherCode(code) {
    // Maps Open-Meteo WMO codes to condition strings for the frontend icon logic
    if (code === 0) return "Clear";
    if (code >= 1 && code <= 3) return "Clouds";
    if (code >= 45 && code <= 48) return "Fog";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
    if ((code >= 71 && code <= 77) || code === 85 || code === 86) return "Snow";
    if (code >= 95 && code <= 99) return "Storm";
    return "Unknown";
}

// Renamed from getWeather to weatherWidget to match your require statement
async function runWeatherWidget() {
    const now = Date.now();
    
    // rate limiting: return cached data if within the 5-minute TTL
    if (weatherCache && (now - lastWeatherFetch < WEATHER_REFRESH)) {
        return weatherCache;
    }
    
    try {
        const config = await loadWeatherConfig();
        const url = `https://api.open-meteo.com/v1/forecast?` + 
          `latitude=${config.WEATHER_LAT}&longitude=${config.WEATHER_LON}&` + 
          `current=temperature_2m,relative_humidity_2m,weather_code,is_day&` + 
          `temperature_unit=fahrenheit&timezone=auto`;
          
        const response = await fetch(url);
        
        if (!response.ok) {
            throw new Error(`API returned status: ${response.status}`);
        }
        
        const data = await response.json();
        const current = data.current;
        
        // determine time of day using the location's actual time and Open-Meteo's solar data
        const localHour = parseInt(current.time.substring(11, 13), 10);
        let timeOfDay = 'day';
        
        if (current.is_day === 0) {
            // API guarantees the sun has set at this specific lat/lon
            timeOfDay = 'night';
        } else if (localHour > 17 && localHour < 20) {
            // It is daytime, but it's late afternoon/early evening in that specific timezone
            timeOfDay = 'sunset';
        }
        
        // parse into JSON format
        weatherCache = {
            location: config.WEATHER_LOCATION_NAME,
            temperature: Math.round(current.temperature_2m),
            condition: decodeWeatherCode(current.weather_code),
            humidity: Math.round(current.relative_humidity_2m),
            timeOfDay: timeOfDay,
            lastUpdated: current.time
        };
        
        lastWeatherFetch = now;
        return weatherCache;
        
    } catch (err) {
        console.error("Failed to fetch weather data:", err.message);
        if (weatherCache) return weatherCache;
        throw err;
    }
}

module.exports = { runWeatherWidget };