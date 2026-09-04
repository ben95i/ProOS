const fsPromises = require('fs/promises');
const path = require('path');

const WEATHER_REFRESH = 5 * 60 * 1000; // cache lifetime, 5 minutes
const SUNSET_FROM_HOUR = 17; // local hours that read as "sunset" while it is
const SUNSET_TO_HOUR = 20;   // still daytime at that latitude

let weatherConfig = null;
let weatherCache = null;
let lastWeatherFetch = 0;

// weather.json holds WEATHER_LAT, WEATHER_LON and WEATHER_LOCATION_NAME
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

// Open-Meteo WMO code -> the condition string the frontend icons key on
function decodeWeatherCode(code) {
    if (code === 0) return "Clear";
    if (code >= 1 && code <= 3) return "Clouds";
    if (code >= 45 && code <= 48) return "Fog";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
    if ((code >= 71 && code <= 77) || code === 85 || code === 86) return "Snow";
    if (code >= 95 && code <= 99) return "Storm";
    return "Unknown";
}

// day / night / sunset, from the location's own clock and Open-Meteo's is_day
function decodeTimeOfDay(current) {
    if (current.is_day === 0) {
        return 'night'; // the API guarantees the sun has set at this lat/lon
    }
    const localHour = parseInt(current.time.substring(11, 13), 10);
    if (localHour > SUNSET_FROM_HOUR && localHour < SUNSET_TO_HOUR) {
        return 'sunset';
    }
    return 'day';
}

/**
 * The current reading, cached for WEATHER_REFRESH:
 * { location, temperature, condition, humidity, timeOfDay, lastUpdated }
 */
async function runWeatherWidget() {
    const now = Date.now();
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
        weatherCache = {
            location: config.WEATHER_LOCATION_NAME,
            temperature: Math.round(current.temperature_2m),
            condition: decodeWeatherCode(current.weather_code),
            humidity: Math.round(current.relative_humidity_2m),
            timeOfDay: decodeTimeOfDay(current),
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
