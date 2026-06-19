//// Automate REST API ////
// by Ben Provenzano III //

const os = require('os');
const path = require('path');
const express = require('express');
const { readFileSync, writeFile } = require('fs');
const { query, validationResult } = require('express-validator');
const { executeMain } = require('./controller');
const { OPCLEDparser } = require('./ledparser');

const app = express();

// Base middleware for typical JSON requests on other routes
app.use(express.json({ limit: '1mb' })); 

const ramDisk = '/var/www/html/ram';
const hostname = os.hostname();
const httpPort = 3000;
const maxReadSize = 256 * 256;
const regex = /^[A-Za-z0-9_.#%@&-]*$/;

const safePath = (filename) => {
    if (filename.includes('..')) throw new Error('directory traversal attempt blocked');
    return path.join(ramDisk, `${filename}.txt`);
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
        const errs = validationResult(req);
        if (!errs.isEmpty()) return res.status(400).json({ errors: errs.array() });
        
        let respout;
        if (action === 'main') {
            respout = await executeMain(ramDisk, arg1, arg2);
            return res.status(200).json(respout);
        }
        if (action === 'leds') {
            respout = await OPCLEDparser(ramDisk, arg1, arg2); 
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
        const errs = validationResult(req);
        if (!errs.isEmpty()) return res.status(400).json({ errors: errs.array() });
        const filePath = safePath(req.query.file);
        const file = readFileSync(filePath, 'utf8');
        res.status(200).json(file.slice(-maxReadSize));
    } catch (err) {
        res.status(500).json(`internal server error: ${err.message}`);
    }
});

// Fixed Race Condition: Use built-in body parsing specific to this route and remove manual stream consumption
app.post('/api/write', express.text({ type: '*/*', limit: '1mb' }), [
    query('file').notEmpty().matches(regex)
], (req, res) => {
    try {
        const errs = validationResult(req);
        if (!errs.isEmpty()) return res.status(400).json({ errors: errs.array() });
        
        const filePath = safePath(req.query.file);
        const body = typeof req.body === 'string' ? req.body : JSON.stringify(req.body);
        
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