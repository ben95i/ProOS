//// Automate REST API ////
// by Ben Provenzano III //
//
// Three routes:
//   GET  /api?action=&arg=&var=   device commands, answered as JSON
//   GET  /api/read?file=          a menu or saved file, as JSON text
//   POST /api/write?file=         writes a saved file (text/plain body)

const os = require('os');
const path = require('path');
const express = require('express');
const { readFileSync, writeFile } = require('fs');
const { query, validationResult } = require('express-validator');
const { executeMain } = require('./controller');
const { OPCLEDparser } = require('./ledparser');

const app = express();

app.use(express.json({ limit: '5mb' }));

const hostname = os.hostname();
const httpPort = 3000;
const maxReadSize = 256 * 256;
const regex = /^[A-Za-z0-9_.#%@&-]*$/;
const baseWWW = '/var/www/html';

// menu files live under menus/, saved state and the log under ram/
const safePath = (subdir, filename) => {
    if (filename.includes('..')) throw new Error('directory traversal attempt blocked');
    return path.join(`${baseWWW}/${subdir}`, `${filename}.txt`);
};

// answers the validation errors, or null when the request is good
const badRequest = (req, res) => {
    const errs = validationResult(req);
    if (errs.isEmpty()) {
        return null;
    }
    return res.status(400).json({ errors: errs.array() });
};

app.get('/api', [
    query('action').notEmpty().matches(regex),
    query('arg').optional().matches(regex),
    query('var').optional().matches(regex)
], async (req, res) => {
    const action = req.query.action;
    const arg1 = req.query.arg || "";
    const arg2 = req.query.var || "";
    try {
        if (badRequest(req, res)) return;
        let respout;
        if (action === 'main') {
            if (arg1 === 'leds') {
                respout = await OPCLEDparser(baseWWW, arg2);
            } else {
                respout = await executeMain(baseWWW, arg1, arg2);
            }
            return res.status(200).json(respout);
        }
        if (action === 'leds') {
            respout = await OPCLEDparser(baseWWW, arg1, arg2);
            return res.status(200).json(respout);
        }
        return res.status(400).json(`invalid action`);
    } catch (err) {
        res.status(500).json(`internal server error: ${err.message}`);
    }
});

app.get('/api/read', [
    query('file').notEmpty().matches(regex)
], (req, res) => {
    try {
        if (badRequest(req, res)) return;
        const fileArg = req.query.file;
        let subdir = 'menus';
        if (fileArg === 'sysout' || fileArg?.includes('.save')) subdir = 'ram';
        const file = readFileSync(safePath(subdir, fileArg), 'utf8');
        res.status(200).json(file.slice(-maxReadSize));
    } catch (err) {
        res.status(500).json(`internal server error: ${err.message}`);
    }
});

app.post('/api/write', express.text({ type: '*/*', limit: '5mb' }), [
    query('file').notEmpty().matches(regex)
], (req, res) => {
    try {
        if (badRequest(req, res)) return;
        const filePath = safePath('ram', req.query.file);
        let body = req.body;
        if (typeof body !== 'string') {
            body = JSON.stringify(req.body);
        }
        writeFile(filePath, body || '', (err) => {
            if (err) return res.status(500).json(`write failed: ${err.message}`);
            res.status(200).end();
        });
    } catch (err) {
        res.status(500).json(`internal server error: ${err.message}`);
    }
});

app.listen(httpPort, '127.0.0.1', () => {
    console.log(`${hostname} API running on port ${httpPort}`);
});
