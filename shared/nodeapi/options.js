// Automate - the API's lists, read once from opts.json beside this file when
// the API starts: the LED hosts, the effects Shuffle picks from, the relax
// sounds and the LAN Scanner's tracked hosts. A change to opts.json takes a
// restart of the API, which every deploy does. A list missing from the file,
// or not a list, is empty and logged, so the rest of the API still starts; a
// file that cannot be read leaves every list empty.

const fs = require('fs');
const path = require('path');

const OPTIONS_FILE = path.join(__dirname, 'opts.json');

// opts.json as an object, or {} with the reason logged
function readOptionsFile() {
    try {
        const fileText = fs.readFileSync(OPTIONS_FILE, 'utf-8');
        return JSON.parse(fileText);
    } catch (error) {
        console.error(`opts.json not read, its lists are empty: ${error.message}`);
        return {};
    }
}

// One list from the file as it stands, or [] with the reason logged.
function optionList(options, listName) {
    const list = options[listName];
    if (!Array.isArray(list)) {
        console.error(`opts.json has no "${listName}" list`);
        return [];
    }
    return list;
}

// a list entry as trimmed text; anything that is not text is ''
function entryText(entry) {
    if (typeof entry !== 'string') {
        return '';
    }
    return entry.trim();
}

// A list of names, trimmed, blank ones dropped:
// nameList({ ledShuffle: ['miami', ' maze ', ''] }, 'ledShuffle') -> ['miami', 'maze']
function nameList(options, listName) {
    const names = [];
    optionList(options, listName).forEach(entry => {
        const name = entryText(entry);
        if (name !== '') {
            names.push(name);
        }
    });
    return Object.freeze(names);
}

// A list of [value, name] pairs as objects keyed valueKey and 'name'. A pair
// without a value is dropped; one without a name is named by its value:
// pairList({ ledHosts: [['http://192.168.1.12:80', 'LEDwall']] }, 'ledHosts', 'url')
//   -> [{ url: 'http://192.168.1.12:80', name: 'LEDwall' }]
function pairList(options, listName, valueKey) {
    const pairs = [];
    optionList(options, listName).forEach(entry => {
        if (!Array.isArray(entry)) {
            return;
        }
        const value = entryText(entry[0]);
        if (value === '') {
            return;
        }
        let name = entryText(entry[1]);
        if (name === '') {
            name = value;
        }
        pairs.push(Object.freeze({ [valueKey]: value, name: name }));
    });
    return Object.freeze(pairs);
}

const OPTIONS = readOptionsFile();

module.exports = {
    // the LED hosts every LED command goes to: [{ url, name }]
    LED_HOSTS: pairList(OPTIONS, 'ledHosts', 'url'),
    // the effects Shuffle picks from: ['miami', ...]
    LED_SHUFFLE: nameList(OPTIONS, 'ledShuffle'),
    // the bedroom Pi's relax sounds: [{ command, name }]
    RELAX_SOUNDS: pairList(OPTIONS, 'relaxSounds', 'command'),
    // the hosts every LAN scan lists, up or down: ['ledwall.home', ...]
    TRACKED_HOSTS: nameList(OPTIONS, 'trackedHosts')
};
