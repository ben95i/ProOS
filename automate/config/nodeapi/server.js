//// Automate REST API ////
// by Ben Provenzano III //

const os = require('os');
const path = require('path');
const express = require('express');
const { readFileSync, writeFile } = require('fs');
const { query, validationResult } = require('express-validator');
const { execFile } = require('child_process');
const { executeMain } = require('./controller');
const { OPCLEDparser } = require('./ledparser');

const app = express();
app.use(express.json({ limit: '1mb' }));
const ramDisk = '/var/www/html/ram';
const hostname = os.hostname();
const httpPort = 3000;
const maxReadSize = 256 * 256;
const maxWriteSize = 1024 * 1024;
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
    let respout;
    const errs = validationResult(req);
    if (!errs.isEmpty()) return res.status(400).json({ errors: errs.array() });
    if (action === 'main') {
        respout = await executeMain(ramDisk, arg1, arg2);
        return res.status(200).json(respout);
    } 
    if (action === 'lrxmit') {
        respout = await executeMain(ramDisk, 'lrxmit', arg1 + '-' + arg2);
        return res.status(200).json(respout);
    }
    if (action === 'brxmit') {
        respout = await executeMain(ramDisk, 'brxmit', arg1 + '-' + arg2);
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

app.post('/api/write', [
  query('file').notEmpty().matches(regex)
  ], (req, res) => {
  try {
    const errs = validationResult(req);
    if (!errs.isEmpty()) return res.status(400).json({ errors: errs.array() });
    const filePath = safePath(req.query.file);
    let body = '';
    let bodySize = 0;
    req.on('data', (data) => {
      bodySize += data.length;
      if (bodySize > maxWriteSize) {
        req.destroy();
        return res.status(413).json("write payload too large");
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