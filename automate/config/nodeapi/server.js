//// Automate REST API ////
// by Ben Provenzano III //

const { readFileSync, writeFile } = require('fs');
const path = require('path');
const { query, validationResult } = require('express-validator');
const express = require('express');
const { execFile } = require('child_process');
const os = require('os');
const { executeMain } = require('./main');
const { executeLeds } = require('./ledparser');

const app = express();
app.use(express.json({ limit: '1mb' })); // Fix 3: Limit JSON body size

const hostname = os.hostname();
const httpPort = 3000;
const ramDisk = '/var/www/html/ram';
const maxReadBytes = 64000;
const MAX_WRITE_SIZE = 1024 * 1024; // Fix 3: 1MB limit for raw data streams
const regex = /^[A-Za-z0-9_.#%@&-]*$/;

// Helper to prevent Directory Traversal (Fix 2)
const safePath = (filename) => {
    if (filename.includes('..')) throw new Error('Directory traversal attempt blocked');
    return path.join(ramDisk, `${filename}.txt`);
};

app.get('/api', [
    query('action').notEmpty().matches(regex),
    query('arg').optional().matches(regex),
    query('var').optional().matches(regex)
  ], async (req, res) => {
  
  const actionRaw = req.query.action;
  const arg1Raw = req.query.arg || "";
  const arg2Raw = req.query.var || "";

  try {
    const errs = validationResult(req);
    if (!errs.isEmpty()) return res.status(400).json({ errors: errs.array() });

    if (actionRaw === 'main' && hostname === 'automate') {
        const message_output = await executeMain(arg1Raw, arg2Raw, ramDisk);
        // Fix 4: Changed status to 200 for successful automation logic
        return res.status(200).json(message_output);
    }
    if (actionRaw === 'leds' && hostname === 'automate') {
        const leds_output = await executeLeds(ramDisk, arg1Raw, arg2Raw); 
        return res.status(200).json(leds_output);
    }
    
    // Fix 1: Use execFile instead of exec to prevent injection
    // Arguments are passed as an array, bypassing shell evaluation
    execFile('/usr/bin/sudo', [`/opt/rpi/${actionRaw}`, arg1Raw, arg2Raw], (err, stdout, stderr) => {
      if (err) {
        res.status(500).json(`command error: ${stderr} ${stdout}`);
      } else {
        res.status(200).json(`${stdout}`);
      }
    });

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

    // Fix 2: Validate path
    const filePath = safePath(req.query.file);
    const file = readFileSync(filePath, 'utf8');
    res.status(200).json(file.slice(-maxReadBytes));
  } catch (err) {
    res.status(500).json(`internal server error: ${err.message}`);
  }
});

app.post('/api/write', [
  query('file').notEmpty().matches(regex)
  ], (req, res) => {
  try {
    const errs = validationResult(req);
    if (!errs.isEmpty()) return res.status(400).json({ errors: errs.array() });

    // Fix 2: Validate path
    const filePath = safePath(req.query.file);
    
    let body = '';
    let bodySize = 0;

    req.on('data', (data) => {
      bodySize += data.length;
      // Fix 3: Enforce size limit during stream
      if (bodySize > MAX_WRITE_SIZE) {
        req.destroy();
        return res.status(413).json("Payload too large");
      }
      body += data;
    });
    
    req.on('end', () => {
      if (res.writableEnded) return;
      writeFile(filePath, body, () => {
        res.status(200).end();
      });
    });
  } catch (err) {
    res.status(500).json(`internal server error: ${err.message}`);
  }
});

app.listen(httpPort, '127.0.0.1', () => {
  console.log(`${hostname} API running on port ${httpPort}`);
});