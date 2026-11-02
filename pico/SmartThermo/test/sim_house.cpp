// =====================================================================================
// test/sim_house.cpp
// SmartThermo host test: the weather and the house the thermostat controls.
//
// Weather: each day has its own mean temperature (fronts move it a few degrees from the season's
// mean) and swing, blended smoothly from noon to noon; the daily curve is coldest about 04:00 and
// warmest about 16:00. Clouds cut the sun on some days.
//
// House: two thermal masses, the indoor air (with furnishings) and the structure. Heat flows
// between the air, the structure and outdoors; the sun, people and appliances add heat; the
// furnace (48,000 BTU/h delivered) and the AC (22,000 BTU/h sensible) ramp up and down with their
// heat exchanger and coil lag; the blower adds its motor heat. The DHT22 sits in a case on a
// wall, so its own temperature lags the air by about 90 s. Humidity follows the outdoor dew
// point through air leakage, rises with cooking and showers, and falls while the AC runs.
// =====================================================================================
#include <math.h>

#include "../common.h"
#include "sim_world.h"

struct SeasonProfile {
  const char *name;
  double meanF;          // season mean outdoor temperature
  double frontF;         // how far a front moves a day's mean
  double swingF;         // day/night difference
  double outdoorDewF;    // outdoor dew point
  double solarPeakBtu;   // sun through the windows at noon on a clear day
};

const SeasonProfile SEASON_PROFILES[SEASON_COUNT] = {
  { "winter", 28.0, 9.0, 14.0, 18.0, 2500.0 },
  { "spring", 63.0, 8.0, 26.0, 47.0, 4500.0 },
  { "summer", 82.0, 5.0, 18.0, 64.0, 6500.0 },
  { "fall", 58.0, 8.0, 24.0, 44.0, 3500.0 },
};

// ---- house (a 1,500 sq ft house) ----
const double ENVELOPE_UA_BTU = 320.0;      // BTU/h per F, air to outdoors (walls, windows, leaks)
const double MASS_UA_BTU = 60.0;           // BTU/h per F, structure to outdoors
const double AIR_MASS_UA_BTU = 3000.0;     // BTU/h per F, air to structure
const double AIR_CAPACITY_BTU = 3500.0;    // BTU per F, air and furnishings
const double MASS_CAPACITY_BTU = 26000.0;  // BTU per F, structure
const double FURNACE_BTU = 48000.0;        // 60,000 BTU/h input at 80 %
const double AC_SENSIBLE_BTU = 22000.0;    // 2.5 ton
const double BLOWER_BTU = 1200.0;          // the blower motor's heat while G runs it
const double SOLAR_TO_AIR = 0.6;           // the rest of the sun warms the structure
const double FURNACE_WARM_UP_S = 75.0;     // heat exchanger warms after W1 closes
const double FURNACE_COOL_DOWN_S = 100.0;  // the furnace blower runs on after W1 opens
const double COIL_COOL_DOWN_S = 100.0;
const double COIL_WARM_UP_S = 45.0;
const double SENSOR_LAG_S = 90.0;
const double LEAKAGE_HOURS = 10.0;         // indoor dew point follows outdoors this slowly
const double COIL_DEW_POINT_F = 48.0;      // the AC dries the air towards this dew point
const double DEHUMIDIFY_HOURS = 1.5;
const double MAX_SUBSTEP_S = 10.0;

// DHT22 frame noise
const double DHT_TEMPERATURE_NOISE_C = 0.08;
const double DHT_HUMIDITY_NOISE = 0.3;

// random number streams (first key of simRandom), so each quantity has its own sequence
const uint64_t RANDOM_FRONT_A = 11;
const uint64_t RANDOM_FRONT_B = 12;
const uint64_t RANDOM_CLOUDS = 13;
const uint64_t RANDOM_DEW = 14;
const uint64_t RANDOM_DHT_TEMPERATURE = 21;
const uint64_t RANDOM_DHT_HUMIDITY = 22;
const uint64_t RANDOM_DHT_INTERMITTENT = 23;
const uint64_t RANDOM_DHT_BIT_FLIP = 24;

const char *seasonName(Season season) {
  if (season >= SEASON_COUNT) {
    return "unknown";
  }
  return SEASON_PROFILES[season].name;
}

static const SeasonProfile &profile() {
  return SEASON_PROFILES[world->config.season];
}

// Returns the hours since midnight of the first day, e.g. 30.5 = day 2, 06:30.
static double worldHours(uint64_t worldUs) {
  double hours = world->config.startHour + (double)worldUs / (double)US_PER_HOUR_SIM;
  return hours;
}

double timeOfDayHours(uint64_t worldUs) {
  double hours = worldHours(worldUs);
  return fmod(hours, 24.0);
}

uint32_t worldDay(uint64_t worldUs) {
  double days = worldHours(worldUs) / 24.0;
  uint32_t dayNumber = (uint32_t)floor(days) + 1;
  return dayNumber;
}

// A day's mean outdoor temperature: the season's mean moved by a front (the sum of two draws
// makes mild days more common than extreme ones).
static double dailyMeanF(int64_t dayIndex) {
  double meanF = profile().meanF;
  double frontF = profile().frontF;
  if (world->weather.active) {
    meanF = world->weather.meanF;
    frontF = 3.0;
  }
  double drawA = simRandom(RANDOM_FRONT_A, (uint64_t)dayIndex, 0);
  double drawB = simRandom(RANDOM_FRONT_B, (uint64_t)dayIndex, 0);
  double front = drawA + drawB - 1.0; // -1..1
  return meanF + front * frontF;
}

static double cloudiness(int64_t dayIndex) {
  return simRandom(RANDOM_CLOUDS, (uint64_t)dayIndex, 0);
}

// A day's swing: clear days swing more than cloudy ones.
static double dailySwingF(int64_t dayIndex) {
  double swingF = profile().swingF;
  if (world->weather.active) {
    swingF = world->weather.swingF;
  }
  double clouds = cloudiness(dayIndex);
  double scale = 1.2 - 0.6 * clouds;
  return swingF * scale;
}

// Returns the outdoor temperature at a world time.
static double outdoorTemperatureF(uint64_t worldUs) {
  double hours = worldHours(worldUs);
  double days = hours / 24.0;
  double sinceFirstNoon = days - 0.5;
  double dayIndexDouble = floor(sinceFirstNoon);
  int64_t dayIndex = (int64_t)dayIndexDouble;
  double fraction = sinceFirstNoon - dayIndexDouble;
  double blend = (1.0 - cos(SIM_PI * fraction)) / 2.0;
  double meanF = dailyMeanF(dayIndex) * (1.0 - blend) + dailyMeanF(dayIndex + 1) * blend;
  double swingF = dailySwingF(dayIndex) * (1.0 - blend) + dailySwingF(dayIndex + 1) * blend;
  double hourOfDay = fmod(hours, 24.0);
  double dailyCurve = -cos(2.0 * SIM_PI * (hourOfDay - 4.0) / 24.0); // -1 at 04:00, +1 at 16:00
  return meanF + swingF / 2.0 * dailyCurve;
}

static int64_t dayIndexAt(uint64_t worldUs) {
  double days = worldHours(worldUs) / 24.0;
  return (int64_t)floor(days);
}

// Sun through the windows, BTU/h: daylight 06:30-19:30, less on cloudy days.
static double solarGainBtu(uint64_t worldUs) {
  double hourOfDay = timeOfDayHours(worldUs);
  bool daylight = (hourOfDay > 6.5) && (hourOfDay < 19.5);
  if (!daylight) {
    return 0.0;
  }
  double sunHeight = sin(SIM_PI * (hourOfDay - 6.5) / 13.0);
  double clouds = cloudiness(dayIndexAt(worldUs));
  double clearSky = 1.0 - 0.65 * clouds;
  return profile().solarPeakBtu * sunHeight * clearSky;
}

// People, cooking and appliances, BTU/h.
static double internalGainBtu(double hourOfDay) {
  double gainBtu = 1000.0;
  bool morning = (hourOfDay >= 6.0) && (hourOfDay < 8.0);
  bool evening = (hourOfDay >= 17.0) && (hourOfDay < 22.0);
  if (morning) {
    gainBtu = gainBtu + 900.0;
  }
  if (evening) {
    gainBtu = gainBtu + 1600.0;
  }
  return gainBtu;
}

// Moisture from showers and cooking, as the dew point it adds over the outdoor one.
static double internalMoistureDewF(double hourOfDay) {
  double addedF = 5.0;
  bool morning = (hourOfDay >= 6.0) && (hourOfDay < 8.0);
  bool evening = (hourOfDay >= 17.0) && (hourOfDay < 20.0);
  if (morning || evening) {
    addedF = addedF + 3.0;
  }
  return addedF;
}

static double outdoorDewPointF(uint64_t worldUs) {
  double draw = simRandom(RANDOM_DEW, (uint64_t)dayIndexAt(worldUs), 0);
  return profile().outdoorDewF + (draw - 0.5) * 8.0;
}

static double fahrenheitToCelsius(double fahrenheit) {
  return (fahrenheit - 32.0) * 5.0 / 9.0;
}

// Relative humidity from the temperature and dew point (Magnus formula), percent.
static double relativeHumidityPercent(double temperatureF, double dewPointF) {
  double temperatureC = fahrenheitToCelsius(temperatureF);
  double dewPointC = fahrenheitToCelsius(dewPointF);
  double vapourAtDew = exp(17.625 * dewPointC / (243.04 + dewPointC));
  double vapourAtAir = exp(17.625 * temperatureC / (243.04 + temperatureC));
  return 100.0 * vapourAtDew / vapourAtAir;
}

// Sets the house to an even indoor temperature and the season's weather at the start.
void houseInit(Season season, double indoorF) {
  (void)season;
  HouseState &house = world->house;
  house.airF = indoorF;
  house.massF = indoorF;
  house.sensorF = indoorF;
  house.outdoorF = outdoorTemperatureF(0);
  house.outdoorDewPointF = outdoorDewPointF(0);
  double startDewF = house.outdoorDewPointF + 8.0;
  double driestF = indoorF - 15.0;
  if (startDewF > driestF) {
    startDewF = driestF;
  }
  house.dewPointF = startDewF;
  house.furnaceOutput = 0.0;
  house.coolingOutput = 0.0;
  house.relativeHumidity = relativeHumidityPercent(house.airF, house.dewPointF);
  house.updatedUs = 0;
}

// Moves an output (0..1) towards on or off with its time constant.
static double rampOutput(double output, bool on, double onSeconds, double offSeconds, double seconds) {
  double target = on ? 1.0 : 0.0;
  double timeConstant = on ? onSeconds : offSeconds;
  double approach = 1.0 - exp(-seconds / timeConstant);
  return output + (target - output) * approach;
}

// Runs the house physics for a few seconds ending at worldUs with the relays given.
static void advanceSeconds(double seconds, bool heatOn, bool coolOn, bool fanOn, uint64_t worldUs) {
  HouseState &house = world->house;
  double hours = seconds / 3600.0;
  bool burnerCalled = heatOn && !house.furnaceLockedOut;
  house.furnaceOutput = rampOutput(house.furnaceOutput, burnerCalled, FURNACE_WARM_UP_S, FURNACE_COOL_DOWN_S, seconds);
  house.coolingOutput = rampOutput(house.coolingOutput, coolOn, COIL_COOL_DOWN_S, COIL_WARM_UP_S, seconds);
  house.outdoorF = outdoorTemperatureF(worldUs);
  house.outdoorDewPointF = outdoorDewPointF(worldUs);
  double hourOfDay = timeOfDayHours(worldUs);
  double solarBtu = solarGainBtu(worldUs);
  double furnaceBtu = house.furnaceOutput * FURNACE_BTU;
  double coolingBtu = house.coolingOutput * AC_SENSIBLE_BTU;
  double blowerBtu = fanOn ? BLOWER_BTU : 0.0;
  double envelopeLossBtu = ENVELOPE_UA_BTU * (house.airF - house.outdoorF);
  double toMassBtu = AIR_MASS_UA_BTU * (house.airF - house.massF);
  double massLossBtu = MASS_UA_BTU * (house.massF - house.outdoorF);
  double airGainBtu = furnaceBtu - coolingBtu + solarBtu * SOLAR_TO_AIR + internalGainBtu(hourOfDay) + blowerBtu
                      - envelopeLossBtu - toMassBtu;
  double massGainBtu = toMassBtu + solarBtu * (1.0 - SOLAR_TO_AIR) - massLossBtu;
  house.airF = house.airF + airGainBtu / AIR_CAPACITY_BTU * hours;
  house.massF = house.massF + massGainBtu / MASS_CAPACITY_BTU * hours;
  double sensorApproach = 1.0 - exp(-seconds / SENSOR_LAG_S);
  house.sensorF = house.sensorF + (house.airF - house.sensorF) * sensorApproach;
  double moistureTargetF = house.outdoorDewPointF + internalMoistureDewF(hourOfDay);
  double leakageApproach = 1.0 - exp(-hours / LEAKAGE_HOURS);
  house.dewPointF = house.dewPointF + (moistureTargetF - house.dewPointF) * leakageApproach;
  if (house.coolingOutput > 0.0) {
    double dryingApproach = 1.0 - exp(-hours * house.coolingOutput / DEHUMIDIFY_HOURS);
    house.dewPointF = house.dewPointF + (COIL_DEW_POINT_F - house.dewPointF) * dryingApproach;
  }
  double wettestF = house.airF - 1.0;
  if (house.dewPointF > wettestF) {
    house.dewPointF = wettestF;
  }
  house.relativeHumidity = relativeHumidityPercent(house.airF, house.dewPointF);
}

// Runs the house from its last update to worldUs with the relays as the pins drive them now.
void houseAdvanceTo(uint64_t worldUs) {
  HouseState &house = world->house;
  if (worldUs <= house.updatedUs) {
    return;
  }
  bool heatOn = simRelayClosed(SIM_PIN_W1);
  bool coolOn = simRelayClosed(SIM_PIN_Y1);
  bool fanOn = simRelayClosed(SIM_PIN_G);
  uint64_t maxSubstepUs = (uint64_t)(MAX_SUBSTEP_S * (double)US_PER_SECOND_SIM);
  while (house.updatedUs < worldUs) {
    uint64_t remainingUs = worldUs - house.updatedUs;
    uint64_t substepUs = remainingUs;
    if (substepUs > maxSubstepUs) {
      substepUs = maxSubstepUs;
    }
    uint64_t substepEndUs = house.updatedUs + substepUs;
    double substepSeconds = (double)substepUs / (double)US_PER_SECOND_SIM;
    advanceSeconds(substepSeconds, heatOn, coolOn, fanOn, substepEndUs);
    house.updatedUs = substepEndUs;
  }
}

// Builds the 5 bytes the DHT22 sends for its own temperature and the humidity (with sensor noise
// and the DHT22's 0.1 C / 0.1 % steps), then applies the fault set for this read:
//   bad checksum: one data bit flipped; out of range: 0xFFFF humidity and 3276.7 C with a valid
//   checksum; intermittent: a one-off checksum or data timeout fault on some reads.
// Example: 20.1 C, 31.2 % -> 01 38 00 C9 02.
void buildDhtFrame(uint8_t frame[5], SensorFault &faultApplied) {
  DhtSim &dht = world->dht;
  HouseState &house = world->house;
  uint64_t readIndex = dht.reads;
  faultApplied = dht.fault;
  bool noFaultSet = (faultApplied == SENSOR_FAULT_NONE);
  if (noFaultSet && dht.intermittentPercent > 0) {
    double draw = simRandom(RANDOM_DHT_INTERMITTENT, readIndex, 0) * 100.0;
    if (draw < (double)dht.intermittentPercent) {
      double kindDraw = simRandom(RANDOM_DHT_INTERMITTENT, readIndex, 1);
      faultApplied = (kindDraw < 0.5) ? SENSOR_FAULT_BAD_CHECKSUM : SENSOR_FAULT_DATA_TIMEOUT;
    }
  }
  double sensorF = house.sensorF;
  if (faultApplied == SENSOR_FAULT_NONE) {
    sensorF = sensorF + dht.spikeF;
    dht.spikeF = 0.0;
  }
  double noiseC = simGaussian(RANDOM_DHT_TEMPERATURE, readIndex, 0) * DHT_TEMPERATURE_NOISE_C;
  if (dht.readingFrozen) {
    sensorF = dht.frozenReadingF;
    noiseC = 0.0;
  }
  double temperatureC = fahrenheitToCelsius(sensorF) + noiseC;
  long temperatureTenthsC = lround(temperatureC * 10.0);
  double humidity = relativeHumidityPercent(house.sensorF, house.dewPointF);
  humidity = humidity + simGaussian(RANDOM_DHT_HUMIDITY, readIndex, 0) * DHT_HUMIDITY_NOISE;
  if (humidity < 0.0) {
    humidity = 0.0;
  }
  if (humidity > 99.9) {
    humidity = 99.9;
  }
  long humidityTenths = lround(humidity * 10.0);
  long temperatureMagnitude = labs(temperatureTenthsC);
  frame[0] = (uint8_t)((humidityTenths >> 8) & 0xFF);
  frame[1] = (uint8_t)(humidityTenths & 0xFF);
  frame[2] = (uint8_t)((temperatureMagnitude >> 8) & 0x7F);
  if (temperatureTenthsC < 0) {
    frame[2] = (uint8_t)(frame[2] | 0x80);
  }
  frame[3] = (uint8_t)(temperatureMagnitude & 0xFF);
  if (faultApplied == SENSOR_FAULT_OUT_OF_RANGE) {
    frame[0] = 0xFF;
    frame[1] = 0xFF;
    frame[2] = 0x7F;
    frame[3] = 0xFF;
  }
  uint32_t byteSum = (uint32_t)frame[0] + frame[1] + frame[2] + frame[3];
  frame[4] = (uint8_t)(byteSum & 0xFF);
  if (faultApplied == SENSOR_FAULT_BAD_CHECKSUM) {
    double bitDraw = simRandom(RANDOM_DHT_BIT_FLIP, readIndex, 0);
    uint8_t flippedBit = (uint8_t)(bitDraw * 32.0);
    uint8_t flippedByte = flippedBit / 8;
    uint8_t bitMask = (uint8_t)(1 << (flippedBit % 8));
    frame[flippedByte] = (uint8_t)(frame[flippedByte] ^ bitMask);
  }
  memcpy(dht.frame, frame, sizeof(dht.frame));
  dht.frameFault = faultApplied;
  dht.frameRoomF = house.airF;
  dht.frameSensorF = sensorF;
  dht.frameHumidity = humidity;
  dht.frameDhtC = (double)temperatureTenthsC / 10.0;
  dht.readPending = true;
}
