// Automate - backup wizard (Server > Options > Backup): a ring on top showing where the
// backup stands (Locked, Ready, the running job's percent, the last job's result), over a
// line of five stages: Backup type (External drives or VMs/LXCs), Attach drive(s), Unlock
// drives, Back-up and Eject drives. A VMs/LXCs backup needs no drives, so with it picked the
// attach and unlock stages are hidden, and the eject stage too unless drives were left
// unlocked.
// Every backup runs as a job on the host (pve tank-backup.sh) through the API's
// /api/backup routes (nodeapi/backup.js); BackupFlow reads the pools' status and the job's
// log (when the window opens, after an action, on Refresh, and every second only while
// the host is busy) and works out the stage the window draws. Never the server log (sysout). Loaded on
// demand by server.js; App.backupWizard while open. A job goes on when the window closes,
// and a window opened later picks it up from its log.

import { promptWindow, onButtons, gridButtonTemplate, textBoxTemplate, makeElement,
  svgElement, showSpinner } from './utils.js';
import { removeDropdowns, spinnerIds } from './dropdowns.js';
import { HttpError, reportError, reportException, showPopup } from './error.js';
import ToggleSwitch from './switch.js';

//// settings ////

const BACKUP_ROUTE = '/api/backup';
// how often the status and log are read while the host is busy (a job runs or holds its
// lock, a job asked for has not begun, or a drive scan is awaited); idle, nothing is read
// until an action or Refresh
const BUSY_POLL_MS = 1000;
// a job the host has not begun this long after it was asked for never started
const JOB_START_WAIT_MS = 20000;
// a drive rescan that has not landed by then is given up on
const RESCAN_WAIT_MS = 15000;
// the most log lines a job model keeps
const MAX_JOB_LINES = 3000;

// the jobs' names on screen: tank-backup.sh's job -> its title
const BACKUP_JOB_TITLES = {
  attach: 'Unlock drives',
  standard: 'Standard backup',
  checksum: 'Checksum backup',
  vms: 'VMs/LXCs backup',
  detach: 'Eject drives'
};
// the phases' names on screen
const BACKUP_PHASE_TITLES = {
  attach: 'Unlocking',
  backup: 'Backing up',
  record: 'Recording dates',
  detach: 'Ejecting'
};
// a job's outcome for a pool (@POOL) on screen, and the ones that went well
const POOL_OUTCOME_TEXT = {
  attached: 'Unlocked',
  already: 'Already unlocked',
  wrongpass: 'Wrong passphrase',
  failed: 'Failed',
  done: 'Backed up',
  incomplete: 'Incomplete',
  skipped: 'Skipped',
  detached: 'Ejected',
  busy: 'Busy, still attached'
};
const POOL_OUTCOME_GOOD = ['attached', 'already', 'done', 'detached'];
// a pool's state (bkpstatus.txt) on screen, as a drive scan writes it into the log
const POOL_STATE_TEXT = {
  attached: 'Unlocked',
  unmounted: 'Imported, not mounted',
  connected: 'Connected, locked',
  absent: 'Not connected'
};
// a step's result (@STEP_END) as an icon
const STEP_RESULT_ICONS = {
  ok: 'fas fa-check',
  warn: 'fas fa-exclamation-triangle',
  failed: 'fas fa-times',
  cancelled: 'fas fa-ban'
};
// the jobs that can be cancelled
const CANCELLABLE_JOBS = ['standard', 'checksum', 'vms'];

//// window settings ////

const WIZARD_ID = 'bkp__prompt';
const REFRESH_BUTTON = spinnerIds('bkp__refresh');
const DRIVE_JOBS = ['standard', 'checksum'];
const EXTERNAL_JOBS = ['attach', 'standard', 'checksum', 'detach'];
// the backup types of stage 1: [type, name, icon]
const BACKUP_TYPES = [
  ['external', 'External drives', 'fad fa-hdd'],
  ['vms', 'VMs/LXCs', 'fad fa-server']
];
// the drive backups of the backup stage: [action, name, icon]
const DRIVE_BACKUPS = [
  ['standard', 'Standard', 'fad fa-sync'],
  ['checksum', 'Checksum', 'fad fa-check-double']
];
// the stages: [key, title, icon]
const STAGES = [
  ['type', 'Backup type', 'fad fa-list-ul'],
  ['attach', 'Attach drive(s)', 'fad fa-plug'],
  ['unlock', 'Unlock drives', 'fad fa-lock-open'],
  ['backup', 'Back-up', 'fad fa-sync'],
  ['eject', 'Eject drives', 'fad fa-eject']
];
// the job phase a working stage shows the steps of
const STAGE_PHASES = { unlock: 'attach', backup: 'backup', eject: 'detach' };
const NODE_ICONS = {
  done: 'fas fa-check',
  failed: 'fas fa-times',
  skipped: 'fas fa-minus'
};
// the ring: its radius in the 120 x 120 drawing, and its length
const RING_RADIUS = 52;
const RING_LENGTH = 2 * Math.PI * RING_RADIUS;
// the ring's word while no job runs: stage -> [icon, word]
const RING_IDLE_WORDS = {
  loading: ['fad fa-hdd', 'Reading'],
  offline: ['fad fa-wifi-slash', 'No answer'],
  locked: ['fad fa-lock', 'Locked'],
  ready: ['fad fa-lock-open', 'Ready'],
  nodrives: ['fad fa-unlink', 'No drives']
};
// the ring's word for the last job's result: result -> [icon, word]
const RING_RESULT_WORDS = {
  ok: ['fad fa-check', 'Done'],
  failed: ['fad fa-exclamation-triangle', 'Problems'],
  cancelled: ['fad fa-ban', 'Cancelled']
};

//// API ////

// Sends one request to the backup routes and answers its JSON, or throws HttpError.
// backupRequest('GET', 'status') -> { running, state, updated, pools }
async function backupRequest(method, routePath, body) {
  const requestOptions = { method: method, cache: 'no-store' };
  if (body !== undefined) {
    requestOptions.headers = { 'Content-Type': 'text/plain' };
    requestOptions.body = body;
  }
  const routeUrl = `${location.protocol}//${location.host}${BACKUP_ROUTE}/${routePath}`;
  const response = await fetch(routeUrl, requestOptions);
  const answerText = await response.text();
  if (!response.ok) {
    throw new HttpError(response.status, answerText);
  }
  return JSON.parse(answerText);
}

//// job model ////

// an empty job model; a new job's log starts a new one
function newJobModel() {
  return {
    logId: null,      // the log file's id (its inode), from the API
    offset: 0,        // bytes of the log read so far
    name: null,       // attach, standard, checksum, vms, detach
    started: 0,       // epoch ms, from @JOB
    phases: [],       // [{ name, steps: [step], pools: { pool: its @POOL outcome } }], in order
    progress: null,   // the last @PROGRESS: { step, overall, transferred, rate, eta }
    cancelRequested: false,
    done: null,       // { result: ok | failed | cancelled, summary }
    lines: []         // the log as text, events written out as sentences
  };
}

// a step's name: "hdd4tb-03 · Media", a guest's "101 Files LXC", or a pool's "hdd4tb-03"
function stepLabel(step) {
  if (step.item === 'attach' || step.item === 'detach') {
    return step.group;
  }
  if (/^[0-9]+$/.test(step.group)) {
    return `${step.group} ${step.item}`;
  }
  return `${step.group} · ${step.item}`;
}

// A number field of an event, or null for "-": eventNumber('45') -> 45
function eventNumber(field) {
  if (field === undefined || field === '-' || field === '') {
    return null;
  }
  const parsedNumber = Number(field);
  if (Number.isNaN(parsedNumber)) {
    return null;
  }
  return parsedNumber;
}

// A text field of an event, '' for "-": eventText('93.66MB/s') -> '93.66MB/s'
function eventText(field) {
  if (field === undefined || field === '-') {
    return '';
  }
  return field;
}

// adds a line to the model's text, keeping its newest MAX_JOB_LINES
function addJobLine(job, text) {
  job.lines.push(text);
  if (job.lines.length > MAX_JOB_LINES) {
    job.lines.splice(0, job.lines.length - MAX_JOB_LINES);
  }
}

// the job's current phase, or null
function currentPhase(job) {
  if (job.phases.length === 0) {
    return null;
  }
  return job.phases[job.phases.length - 1];
}

// the step of the job's current phase that started last, or null
function currentStep(job) {
  const phase = currentPhase(job);
  if (phase === null) {
    return null;
  }
  let lastStep = null;
  phase.steps.forEach(step => {
    if (step) {
      lastStep = step;
    }
  });
  return lastStep;
}

// the phase of a job with a given name, or null: jobPhase(job, 'backup')
function jobPhase(job, phaseName) {
  const foundPhase = job.phases.find(phase => phase.name === phaseName);
  if (foundPhase === undefined) {
    return null;
  }
  return foundPhase;
}

// Reads one log line into the model: an "@" event updates it (and is written out as a
// sentence in its text), any other line is text.
// readLogLine(job, '@STEP|2|5|hdd4tb-03|Data')
function readLogLine(job, line) {
  if (line.charAt(0) !== '@') {
    addJobLine(job, line);
    return;
  }
  const fields = line.substring(1).split('|');
  const phase = currentPhase(job);
  switch (fields[0]) {
    case 'JOB': {
      job.name = fields[1];
      job.started = Number(fields[2]);
      const jobTitle = BACKUP_JOB_TITLES[job.name] || job.name;
      addJobLine(job, `=== ${jobTitle} ===`);
      break;
    }
    case 'PHASE': {
      job.phases.push({ name: fields[1], steps: [], pools: {} });
      job.progress = null;
      const phaseTitle = BACKUP_PHASE_TITLES[fields[1]] || fields[1];
      addJobLine(job, `--- ${phaseTitle} ---`);
      break;
    }
    case 'STEP': {
      if (phase === null) {
        break;
      }
      const step = {
        n: Number(fields[1]),
        count: Number(fields[2]),
        group: fields[3],
        item: fields[4],
        percent: null,
        result: null,
        detail: ''
      };
      phase.steps[step.n - 1] = step;
      const doneSteps = step.n - 1;
      const overallPercent = Math.floor((doneSteps * 100) / step.count);
      job.progress = { step: null, overall: overallPercent, transferred: '', rate: '', eta: '' };
      addJobLine(job, `> step ${step.n} of ${step.count}: ${stepLabel(step)}`);
      break;
    }
    case 'PROGRESS': {
      job.progress = {
        step: eventNumber(fields[1]),
        overall: eventNumber(fields[2]),
        transferred: eventText(fields[3]),
        rate: eventText(fields[4]),
        eta: eventText(fields[5])
      };
      const runningStep = currentStep(job);
      if (runningStep !== null) {
        runningStep.percent = job.progress.step;
      }
      break;
    }
    case 'STEP_END': {
      if (phase === null) {
        break;
      }
      const endedStep = phase.steps[Number(fields[1]) - 1];
      if (!endedStep) {
        break;
      }
      endedStep.result = fields[2];
      endedStep.detail = fields[3] || '';
      // the phase's overall percent counts the ended step whole
      const endedPercent = Math.floor((endedStep.n * 100) / endedStep.count);
      job.progress = { step: null, overall: endedPercent, transferred: '', rate: '', eta: '' };
      let endText = `  ${endedStep.result}`;
      if (endedStep.detail) {
        endText = `${endText}: ${endedStep.detail}`;
      }
      addJobLine(job, endText);
      break;
    }
    case 'POOL': {
      // kept per phase: a backup's own eject phase reports the same drives again
      if (phase !== null) {
        phase.pools[fields[1]] = fields[2];
      }
      const outcomeText = POOL_OUTCOME_TEXT[fields[2]] || fields[2];
      addJobLine(job, `  ${fields[1]}: ${outcomeText}`);
      break;
    }
    case 'CANCEL':
      job.cancelRequested = true;
      addJobLine(job, '!! cancel requested');
      break;
    case 'REFUSED':
      addJobLine(job, `!! ${fields[1]} refused: ${fields[2]} is running`);
      break;
    case 'DONE':
      job.done = { result: fields[2], summary: fields[3] || '' };
      addJobLine(job, `=== ${fields[2]}: ${job.done.summary} ===`);
      break;
  }
}

//// flow ////

/**
 * The window's view of the backup: reads the status and the job log, and works out the
 * stage to draw. It reads once on open(), again after every action and refresh(), and
 * every BUSY_POLL_MS while the host is busy (hostBusy()); idle, it reads nothing.
 * new BackupFlow(onChange) then open(); onChange(flow) is called after every read and
 * action. close() stops it.
 *
 * stage: loading   nothing read yet, or the host has not scanned the drives yet
 *        offline   the API did not answer
 *        running   a job runs, or one asked for has not begun yet (pending)
 *        finished  a job this window watched has ended, until acknowledge()
 *        ready     no job runs and a drive is attached
 *        locked    no drive attached, one connected (or imported but not mounted)
 *        nodrives  no backup drive connected
 */
class BackupFlow {

  constructor(onChange) {
    this.onChange = onChange;
    this.status = null;
    this.job = newJobModel();
    this.offline = false;
    // the action asked for and not begun yet: { jobName, title, requested }
    this.pending = null;
    // a request to start an action is on its way, so a second press sends nothing
    this.sending = false;
    // a rescan asked for: { asked, updated }, the status time it waits to change from
    this.rescan = null;
    // Refresh asked for a read of the host's state, and it has not landed yet
    this.refreshing = false;
    // the drive scans' lines written into the log since the job on show began; a new
    // job's log starts with them, as the host writes nothing for a scan
    this.scanLines = [];
    // jobs started at or before this were over before this window looked
    this.acknowledgedStarted = null;
    this.closed = false;
    // ends the wait for the next read early; wakeAsked keeps a wake asked for mid-read
    this.wakeUp = null;
    this.wakeAsked = false;
  }

  // starts reading
  open() {
    this.pollLoop();
  }

  // stops reading; a late answer is dropped
  close() {
    this.closed = true;
    this.wake();
  }

  // reads now: an action, Refresh, or close()
  wake() {
    this.wakeAsked = true;
    if (this.wakeUp !== null) {
      this.wakeUp();
    }
  }

  // true while the host is working on something this window waits for: a job runs (or
  // holds its lock while it rewrites the drives' state after its last line, which a
  // stale status keeps true through a dropped connection), a job asked for has not
  // begun, a drive scan is awaited, or the first status (the API asked for a scan)
  hostBusy() {
    const lockHeld = this.status !== null && this.status.running !== null;
    const stage = this.stage;
    if (lockHeld || this.pending !== null || this.rescan !== null) {
      return true;
    }
    return stage === 'running' || stage === 'loading';
  }

  // Reads the status and the log until closed: every BUSY_POLL_MS while the host is
  // busy, otherwise only when woken (an action, Refresh, or close()).
  async pollLoop() {
    while (!this.closed) {
      this.wakeAsked = false;
      // a Refresh pressed during a read is answered by the next one
      const readAnswersRefresh = this.refreshing;
      await this.readAll();
      if (this.closed) {
        return;
      }
      if (readAnswersRefresh) {
        this.refreshing = false;
      }
      this.notify();
      if (this.wakeAsked) {
        continue;
      }
      const hostBusy = this.hostBusy();
      await new Promise(resolve => {
        let pollTimer = null;
        if (hostBusy) {
          pollTimer = setTimeout(resolve, BUSY_POLL_MS);
        }
        this.wakeUp = () => {
          if (pollTimer !== null) {
            clearTimeout(pollTimer);
          }
          resolve();
        };
      });
      this.wakeUp = null;
    }
  }

  // tells the window something changed
  notify() {
    if (!this.closed) {
      this.onChange(this);
    }
  }

  // Reads the status and the new log lines; sets offline when the API did not answer.
  async readAll() {
    try {
      const [statusAnswer] = await Promise.all([
        backupRequest('GET', 'status'),
        this.readLog()
      ]);
      this.status = statusAnswer;
      this.offline = false;
    } catch (error) {
      if (!this.offline) {
        reportError('reading the backup status', error);
      }
      this.offline = true;
      return;
    }
    this.settlePending();
    this.settleRescan();
  }

  // Reads the log from where the last read ended, starting a new model when the API
  // says a new job's log began. A long log comes in chunks, read on to its end.
  async readLog() {
    const logIdText = this.job.logId || '';
    let chunk = await backupRequest('GET', `log?id=${logIdText}&offset=${this.job.offset}`);
    if (chunk.reset === true) {
      this.job = newJobModel();
      this.scanLines.forEach(scanLine => addJobLine(this.job, scanLine));
      this.scanLines = [];
    }
    while (true) {
      this.job.logId = chunk.id;
      this.job.offset = chunk.offset;
      chunk.text.split('\n').forEach(line => {
        if (line !== '') {
          readLogLine(this.job, line);
        }
      });
      if (chunk.offset >= chunk.size || chunk.text === '') {
        break;
      }
      chunk = await backupRequest('GET', `log?id=${chunk.id}&offset=${chunk.offset}`);
    }
    // the job on show when the window opened is not one it watched end
    if (this.acknowledgedStarted === null) {
      this.acknowledgedStarted = 0;
      if (this.job.done !== null) {
        this.acknowledgedStarted = this.job.started;
      }
    }
  }

  // Ends the wait for a job asked for: begun, or never started.
  settlePending() {
    if (this.pending === null) {
      return;
    }
    if (this.job.started >= this.pending.requested) {
      this.pending = null;
      return;
    }
    const waitedMs = Date.now() - this.pending.requested;
    if (waitedMs > JOB_START_WAIT_MS) {
      const jobTitle = this.pending.title.toLowerCase();
      this.pending = null;
      reportException(`starting the ${jobTitle}`,
        'The server did not start the job. Is its file monitor running?');
    }
  }

  // Ends a rescan once the host has written a new status, or given up, and writes what
  // it found into the log.
  settleRescan() {
    if (this.rescan === null || this.status === null) {
      return;
    }
    const waitedMs = Date.now() - this.rescan.asked;
    if (this.status.updated !== this.rescan.updated) {
      this.rescan = null;
      this.logScanResult();
    } else if (waitedMs > RESCAN_WAIT_MS) {
      this.rescan = null;
      this.addScanLine('  no answer from the server');
    }
  }

  // Adds a drive scan's line to the log on show, kept for the next job's log too.
  // addScanLine('--- Looking for drives ---')
  addScanLine(text) {
    addJobLine(this.job, text);
    this.scanLines.push(text);
  }

  // Writes each drive's state after a scan, then how many are connected:
  // "  hdd4tb-03: Connected, locked", "  1 drive(s) connected"
  logScanResult() {
    this.status.pools.forEach(pool => {
      const stateText = POOL_STATE_TEXT[pool.state] || pool.state;
      this.addScanLine(`  ${pool.name}: ${stateText}`);
    });
    const connectedPools = this.status.pools.filter(pool => pool.state !== 'absent');
    if (connectedPools.length === 0) {
      this.addScanLine('  no backup drive connected');
    } else {
      this.addScanLine(`  ${connectedPools.length} drive(s) connected`);
    }
  }

  // where the backup stands (see the class comment)
  get stage() {
    if (this.offline) {
      return 'offline';
    }
    // updated is null until the host has scanned the drives (the API asked it to)
    if (this.status === null || this.status.updated === null) {
      return 'loading';
    }
    if (this.pending !== null) {
      return 'running';
    }
    const lockedJob = this.status.running;
    const lockedJobDone = this.job.done !== null && this.job.name === lockedJob;
    if (lockedJob !== null && !lockedJobDone) {
      return 'running';
    }
    if (this.job.done !== null && this.job.started > this.acknowledgedStarted) {
      return 'finished';
    }
    if (this.poolsIn('attached').length > 0) {
      return 'ready';
    }
    if (this.poolsIn('connected').length > 0 || this.poolsIn('unmounted').length > 0) {
      return 'locked';
    }
    return 'nodrives';
  }

  // the running job's name, the one asked for while it has not begun, or null
  get runningJob() {
    if (this.pending !== null) {
      return this.pending.jobName;
    }
    if (this.status !== null && this.status.running) {
      return this.status.running;
    }
    return null;
  }

  // the pools in one state: poolsIn('attached') -> [{ name, state, lastBackup, shares }]
  poolsIn(poolState) {
    if (this.status === null) {
      return [];
    }
    return this.status.pools.filter(pool => pool.state === poolState);
  }

  // the newest last backup of any pool ("2026-11-20 15:12"), or null
  get lastBackup() {
    if (this.status === null) {
      return null;
    }
    const backupDates = this.status.pools
      .map(pool => pool.lastBackup)
      .filter(lastBackup => lastBackup && lastBackup !== 'never')
      .sort();
    if (backupDates.length === 0) {
      return null;
    }
    return backupDates[backupDates.length - 1];
  }

  // the job's overall percent in its current phase (0 when unknown, and while a job asked
  // for has not begun, as the model still holds the one before)
  get overallPercent() {
    if (this.pending !== null || this.job.progress === null || this.job.progress.overall === null) {
      return 0;
    }
    return this.job.progress.overall;
  }

  // what the running job is doing: "Backing up", "Cancelling...", "Waiting for the server..."
  get phaseText() {
    if (this.job.cancelRequested && this.job.done === null) {
      return 'Cancelling...';
    }
    const phase = currentPhase(this.job);
    if (this.pending !== null || phase === null) {
      return 'Waiting for the server...';
    }
    return BACKUP_PHASE_TITLES[phase.name] || phase.name;
  }

  // The current step's detail: "220GiB · 93.66MB/s · 0:02:22 left", or ''.
  get progressDetail() {
    const progress = this.job.progress;
    if (progress === null || this.pending !== null) {
      return '';
    }
    const detailParts = [];
    if (progress.transferred) {
      detailParts.push(progress.transferred);
    }
    if (progress.rate) {
      detailParts.push(progress.rate);
    }
    if (progress.eta) {
      detailParts.push(`${progress.eta} left`);
    }
    return detailParts.join(' · ');
  }

  // true while the running job can be cancelled: a backup or VM backup not already stopping
  get canCancel() {
    if (this.stage !== 'running' || this.pending !== null) {
      return false;
    }
    return CANCELLABLE_JOBS.includes(this.runningJob) && !this.job.cancelRequested;
  }

  // the job's log as text, newest line last
  get logText() {
    return this.job.lines.join('\n');
  }

  // Asks the API for an action, then waits for its job to begin; answers true when it
  // was started. A busy answer is shown as a notice; a press while one is on its way
  // sends nothing.
  async start(action, body) {
    if (this.sending) {
      return false;
    }
    this.sending = true;
    let answer = null;
    try {
      answer = await backupRequest('POST', action, body);
    } catch (error) {
      reportError(`starting the backup action ${action}`, error);
      return false;
    } finally {
      this.sending = false;
    }
    if (answer.result === 'busy') {
      const runningTitle = BACKUP_JOB_TITLES[answer.running] || answer.running;
      showPopup(`[backup] ${runningTitle} is running; try again once it finishes.`);
      this.wake();
      return false;
    }
    const startsJob = action !== 'cancel' && action !== 'status';
    if (answer.result === 'started' && startsJob) {
      const jobName = action.replace('_eject', '');
      this.pending = {
        jobName: jobName,
        title: BACKUP_JOB_TITLES[jobName] || action,
        requested: answer.requested
      };
    }
    this.notify();
    this.wake();
    return true;
  }

  // Unlocks (attaches) the connected drives with a passphrase.
  unlock(passphrase) {
    return this.start('attach', passphrase);
  }

  // Runs a backup: backupAction standard, checksum or vms; ejectAfter detaches the
  // drives after a drive backup.
  backup(backupAction, ejectAfter) {
    let action = backupAction;
    if (ejectAfter === true && backupAction !== 'vms') {
      action = `${backupAction}_eject`;
    }
    return this.start(action);
  }

  // stops the running backup or VM backup
  cancel() {
    return this.start('cancel');
  }

  // detaches the attached drives
  eject() {
    return this.start('detach');
  }

  // Reads the status and the log now, as the host last wrote them, asking it for
  // nothing (Refresh, which spins until the read lands).
  refresh() {
    this.refreshing = true;
    this.notify();
    this.wake();
  }

  // asks the host to scan the drives again (the attach stage's check button); the log
  // gets the scan's heading (once while one is awaited) and, later, what it found
  rescanDrives() {
    let updatedBefore = null;
    if (this.status !== null) {
      updatedBefore = this.status.updated;
    }
    if (this.rescan === null) {
      this.addScanLine('--- Looking for drives ---');
    }
    this.rescan = { asked: Date.now(), updated: updatedBefore };
    return this.start('status');
  }

  // the finished job has been seen: the stage goes back to the drives
  acknowledge() {
    this.acknowledgedStarted = this.job.started;
    this.notify();
  }
}

//// window ////

// Opens the wizard and starts reading the backup's state.
export function openBackupWizard() {
  if (App.backupWizard) {
    return;
  }
  removeDropdowns();
  const stageRows = STAGES.map(([key, title, icon]) => `
    <div class="bkp__stage" id="bkp__stage-${key}">
      <div class="bkp__node"><span class="bkp__num"></span><i class="bkp__node-icon"></i></div>
      <div class="bkp__content">
        <div class="bkp__head"><i class="${icon}"></i><span class="bkp__stage-title">${title}</span></div>
        <div class="bkp__sub bkp__small" id="bkp__sub-${key}"></div>
        <div class="bkp__body" id="bkp__body-${key}"></div>
      </div>
    </div>`).join('');
  const win = promptWindow(WIZARD_ID, `
    <div class="bkp__title">Backup</div>
    <div class="bkp__ring" id="bkp__ring">
      <div class="bkp__centre">
        <i id="bkp__centre-icon"></i>
        <span class="bkp__centre-big" id="bkp__centre-big"></span>
        <span class="bkp__centre-small bkp__tiny" id="bkp__centre-small"></span>
      </div>
    </div>
    <div class="bkp__line">${stageRows}</div>
    <div id="bkp__logslot"></div>
    <div class="bkp__actions">
      ${gridButtonTemplate({ buttonId: 'bkp__close', icon: 'fas fa-times', buttonClass: 'bkp__action' })}
      ${gridButtonTemplate({ buttonId: 'bkp__log', icon: 'fad fa-terminal', buttonClass: 'bkp__action' })}
      ${gridButtonTemplate({
        buttonId: 'bkp__refresh',
        icon: 'fad fa-sync-alt',
        buttonClass: 'bkp__action',
        ...REFRESH_BUTTON,
        iconSpinner: true
      })}
    </div>
  `);
  const wizard = {
    win: win,
    flow: null,
    ring: document.getElementById('bkp__ring'),
    ringArc: null,
    // stage 1's pick: external or vms
    backupType: 'external',
    // the attach stage's check button was pressed (a drive scan was asked for)
    attachChecked: false,
    // the backup stage's pick for a drive backup: standard or checksum
    driveBackup: 'standard',
    ejectAfter: true,
    ejectSwitch: null,
    showLog: false,
    // the stages' controls on screen, so they are drawn again only when they change
    drawnControls: '',
    // what this window saw end: the last backup's and eject's outcomes
    lastBackupDone: null,
    lastEjectDone: null,
    // the start time of the job whose result the ring no longer shows (a type was picked)
    ringClearedStarted: null
  };
  drawRing(wizard);
  wizard.flow = new BackupFlow(() => drawWizard(wizard));
  App.backupWizard = wizard;
  onButtons(win, button => pressButton(wizard, button));
  wizard.flow.open();
  drawWizard(wizard);
}

// Closes the wizard; a running job goes on on the server.
export function closeBackupWizard() {
  const wizard = App.backupWizard;
  if (!wizard) {
    return;
  }
  App.backupWizard = null;
  wizard.flow.close();
  dropEjectSwitch(wizard);
  wizard.win.remove();
}

//// ring ////

// draws the ring's track and arc, as the room status light draws its countdown
function drawRing(wizard) {
  const svg = svgElement('svg', { viewBox: '0 0 120 120', class: 'bkp__svg' });
  const circleShape = { cx: '60', cy: '60', r: String(RING_RADIUS) };
  svg.appendChild(svgElement('circle', { ...circleShape, class: 'bkp__track' }));
  wizard.ringArc = svgElement('circle', {
    ...circleShape,
    class: 'bkp__arc',
    'stroke-dasharray': String(RING_LENGTH),
    'stroke-dashoffset': String(RING_LENGTH)
  });
  svg.appendChild(wizard.ringArc);
  wizard.ring.insertBefore(svg, wizard.ring.firstChild);
}

// fills the ring to a percent
function setRing(wizard, percent) {
  const filledLength = (RING_LENGTH * percent) / 100;
  const emptyLength = RING_LENGTH - filledLength;
  wizard.ringArc.setAttribute('stroke-dashoffset', String(emptyLength));
}

// The ring shows the last job's result: one this window watched end, or one that ended
// before it opened (the wizard closed meanwhile), until a new job starts or a backup
// type is picked.
function ringShowsResult(wizard) {
  const flow = wizard.flow;
  const stage = flow.stage;
  if (stage === 'finished') {
    return true;
  }
  const idleStages = ['ready', 'locked', 'nodrives'];
  if (!idleStages.includes(stage) || flow.job.done === null) {
    return false;
  }
  return flow.job.started !== wizard.ringClearedStarted;
}

// A job's start time as the drives' dates are written: 1795819739717 -> "2026-11-27 17:38"
function startTimeText(epochMs) {
  const startDate = new Date(epochMs);
  const year = startDate.getFullYear();
  const month = String(startDate.getMonth() + 1).padStart(2, '0');
  const day = String(startDate.getDate()).padStart(2, '0');
  const hours = String(startDate.getHours()).padStart(2, '0');
  const minutes = String(startDate.getMinutes()).padStart(2, '0');
  return `${year}-${month}-${day} ${hours}:${minutes}`;
}

// Draws the ring: the running job's percent and phase, the last job's result with its
// start time, or where the drives stand with the last backup under it.
function drawGauge(wizard) {
  const flow = wizard.flow;
  const stage = flow.stage;
  wizard.ring.className = `bkp__ring is-${stage}`;
  let icon = '';
  let big = '';
  let small = '';
  let ringPercent = 0;
  if (stage === 'running') {
    big = `${flow.overallPercent}%`;
    small = flow.phaseText;
    ringPercent = flow.overallPercent;
  } else if (ringShowsResult(wizard)) {
    const jobResult = flow.job.done.result;
    [icon, big] = RING_RESULT_WORDS[jobResult] || RING_RESULT_WORDS.failed;
    wizard.ring.classList.add(`is-${jobResult}`);
    small = `started ${startTimeText(flow.job.started)}`;
    ringPercent = 100;
  } else if (wizard.backupType === 'vms' && stage !== 'loading' && stage !== 'offline') {
    // a VM backup is ready whatever the drives are doing
    icon = 'fad fa-server';
    big = 'Ready';
  } else {
    [icon, big] = RING_IDLE_WORDS[stage];
    if (flow.lastBackup) {
      small = `last ${flow.lastBackup}`;
    }
  }
  setRing(wizard, ringPercent);
  document.getElementById('bkp__centre-icon').className = icon;
  document.getElementById('bkp__centre-big').textContent = big;
  document.getElementById('bkp__centre-small').textContent = small;
}

//// stages ////

// Keeps what the job on show says: the backup type a running job, or the one whose result
// the ring shows, belongs to, and the last backup and eject outcomes, which stay on the
// line after the stage moves on.
function noteJob(wizard) {
  const flow = wizard.flow;
  const job = flow.job;
  // the job running, or the one whose result the ring shows (until a type is picked)
  let shownJob = flow.runningJob;
  if (shownJob === null && ringShowsResult(wizard)) {
    shownJob = job.name;
  }
  if (shownJob === 'vms') {
    wizard.backupType = 'vms';
  } else if (EXTERNAL_JOBS.includes(shownJob)) {
    wizard.backupType = 'external';
  }
  if (job.done === null) {
    return;
  }
  if (DRIVE_JOBS.includes(job.name) || job.name === 'vms') {
    wizard.lastBackupDone = { name: job.name, started: job.started, ...job.done };
  }
  if (job.name === 'detach') {
    wizard.lastEjectDone = { started: job.started, ...job.done };
  }
  // a backup with Eject when finished ends with its own eject phase
  const ejectPhase = jobPhase(job, 'detach');
  if (job.name !== 'detach' && ejectPhase !== null) {
    wizard.lastEjectDone = ejectOutcome(job, ejectPhase);
  }
}

// The eject phase of a backup with Eject when finished, as a detach job's outcome:
// { started, result, summary }, ok when every drive was ejected.
function ejectOutcome(job, ejectPhase) {
  const ejectSteps = ejectPhase.steps.filter(step => step);
  const ejectedSteps = ejectSteps.filter(step => step.result === 'ok');
  let ejectResult = 'failed';
  if (ejectSteps.length > 0 && ejectedSteps.length === ejectSteps.length) {
    ejectResult = 'ok';
  }
  const ejectSummary = `${ejectedSteps.length} of ${ejectSteps.length} drive(s) ejected`;
  return { started: job.started, result: ejectResult, summary: ejectSummary };
}

// a result as a stage state: ok -> done, anything else -> failed
function resultState(result) {
  if (result === 'ok') {
    return 'done';
  }
  return 'failed';
}

// The attach stage: { state, text, open, hidden }. Done once a drive is connected, or while
// a job runs or this window shows one it watched end (it got past this stage); otherwise
// open, with the check button that asks the host to look for the drive, and failed (still
// open) when it looked and found none. A result from before the window opened does not
// count: with its drive gone the stage asks for one again.
function attachState(wizard, driveCount) {
  const flow = wizard.flow;
  const stage = flow.stage;
  if (wizard.backupType === 'vms') {
    return { state: 'skipped', hidden: true };
  }
  let connectedText = '';
  if (driveCount > 0) {
    connectedText = `${driveCount} drive(s) connected`;
  }
  if (stage === 'running' || stage === 'finished' || driveCount > 0) {
    return { state: 'done', text: connectedText };
  }
  if (stage === 'loading') {
    return { state: 'waiting', text: 'Reading the drives...' };
  }
  if (stage === 'offline') {
    return { state: 'waiting', text: '' };
  }
  if (flow.rescan !== null) {
    return { state: 'working', text: 'Looking for the drives...' };
  }
  if (wizard.attachChecked) {
    return { state: 'failed', text: 'No backup drive(s) found', open: true };
  }
  return { state: 'open', text: '' };
}

// The unlock stage: { state, text, open, hidden }
function unlockState(wizard, runningJob, attachedCount, lockedCount) {
  const flow = wizard.flow;
  const job = flow.job;
  const unlockFailed = job.name === 'attach' && job.done !== null && job.done.result !== 'ok';
  if (wizard.backupType === 'vms') {
    return { state: 'skipped', hidden: true };
  }
  if (runningJob === 'attach') {
    return { state: 'working', text: flow.phaseText };
  }
  if (attachedCount > 0) {
    // an unlock that left a drive behind says so
    let unlockText = `${attachedCount} drive(s) unlocked`;
    if (unlockFailed) {
      unlockText = job.done.summary;
    }
    return { state: 'done', text: unlockText };
  }
  if (flow.stage === 'finished' && unlockFailed) {
    return { state: 'failed', text: job.done.summary, open: lockedCount > 0 };
  }
  if (lockedCount > 0) {
    return { state: 'open', text: `${lockedCount} drive(s) connected and locked` };
  }
  // the attach stage asks for the drive
  return { state: 'waiting', text: '' };
}

// A drive backup's outcome once its own eject phase has begun (Eject when finished),
// from its backup phase's @POOL outcomes: { state: done or failed, text: "2 of 3 drive(s) backed up" }
function backupPhaseState(job) {
  const backupPhase = jobPhase(job, 'backup');
  let poolOutcomes = [];
  if (backupPhase !== null) {
    poolOutcomes = Object.values(backupPhase.pools);
  }
  const donePools = poolOutcomes.filter(outcome => outcome === 'done');
  let phaseState = 'failed';
  if (poolOutcomes.length > 0 && donePools.length === poolOutcomes.length) {
    phaseState = 'done';
  }
  return { state: phaseState, text: `${donePools.length} of ${poolOutcomes.length} drive(s) backed up` };
}

// true while a drive backup runs its own eject phase (Eject when finished)
function ejectingAfterBackup(flow, runningJob) {
  const phase = currentPhase(flow.job);
  if (!DRIVE_JOBS.includes(runningJob) || flow.pending !== null || phase === null) {
    return false;
  }
  return phase.name === 'detach';
}

// The backup stage: { state, text, open }. A result stays on the line, with Start again
// while there is something to back up to.
function backupState(wizard, runningJob, attachedCount) {
  const flow = wizard.flow;
  const stage = flow.stage;
  const vmsPicked = wizard.backupType === 'vms';
  const backupDone = wizard.lastBackupDone;
  let backupDoneHere = false;
  if (backupDone !== null) {
    const doneVms = backupDone.name === 'vms';
    backupDoneHere = doneVms === vmsPicked;
  }
  if (ejectingAfterBackup(flow, runningJob)) {
    return backupPhaseState(flow.job);
  }
  if (DRIVE_JOBS.includes(runningJob) || runningJob === 'vms') {
    return { state: 'working', text: flow.phaseText };
  }
  if (stage === 'loading' || stage === 'offline') {
    return { state: 'waiting', text: '' };
  }
  // the result stays through a later unlock or eject, without Start while that runs
  if (backupDoneHere) {
    const canStartAgain = stage !== 'running' && (vmsPicked || attachedCount > 0);
    return { state: resultState(backupDone.result), text: backupDone.summary, open: canStartAgain };
  }
  if (stage === 'running') {
    return { state: 'waiting', text: '' };
  }
  if (vmsPicked) {
    return { state: 'open', text: '' };
  }
  if (attachedCount === 0) {
    return { state: 'waiting', text: '' };
  }
  return { state: 'open', text: 'Choose Standard or Checksum' };
}

// The eject stage: { state, text, hidden }
function ejectState(wizard, runningJob, attachedCount) {
  const flow = wizard.flow;
  const jobRunning = flow.stage === 'running';
  const ejectDone = wizard.lastEjectDone;
  if (runningJob === 'detach' || ejectingAfterBackup(flow, runningJob)) {
    return { state: 'working', text: flow.phaseText };
  }
  if (attachedCount > 0 && !jobRunning) {
    return { state: 'open', text: `${attachedCount} drive(s) still unlocked` };
  }
  if (wizard.backupType === 'vms' && attachedCount === 0) {
    return { state: 'skipped', hidden: true };
  }
  if (ejectDone !== null && attachedCount === 0 && !jobRunning) {
    return { state: resultState(ejectDone.result), text: ejectDone.summary };
  }
  return { state: 'waiting', text: '' };
}

// Works out each stage's state: waiting, open (its controls), working, done, failed or
// skipped, its line of text, and whether it is hidden.
function stageStates(wizard) {
  const flow = wizard.flow;
  noteJob(wizard);
  let runningJob = null;
  if (flow.stage === 'running') {
    runningJob = flow.runningJob;
  }
  const attachedCount = flow.poolsIn('attached').length;
  const lockedCount = flow.poolsIn('connected').length + flow.poolsIn('unmounted').length;
  const driveCount = attachedCount + lockedCount;
  return {
    // always picked, so always done; the chips say which
    type: { state: 'done', text: '' },
    attach: attachState(wizard, driveCount),
    unlock: unlockState(wizard, runningJob, attachedCount, lockedCount),
    backup: backupState(wizard, runningJob, attachedCount),
    eject: ejectState(wizard, runningJob, attachedCount)
  };
}

// Draws the ring and the stages: each node's state and line, and the open or working
// stages' bodies.
function drawWizard(wizard) {
  const flow = wizard.flow;
  const states = stageStates(wizard);
  drawGauge(wizard);
  // the stages on show, numbered in order; the last draws no line down
  const shownKeys = STAGES.map(([key]) => key).filter(key => states[key].hidden !== true);
  const lastShownKey = shownKeys[shownKeys.length - 1];
  STAGES.forEach(([key]) => {
    const stageState = states[key];
    const row = document.getElementById(`bkp__stage-${key}`);
    const stageNumber = shownKeys.indexOf(key) + 1;
    const nodeIcon = NODE_ICONS[stageState.state] || '';
    row.hidden = stageState.hidden === true;
    row.className = `bkp__stage is-${stageState.state}`;
    row.classList.toggle('is-last', key === lastShownKey);
    row.querySelector('.bkp__num').textContent = String(stageNumber);
    row.querySelector('.bkp__node-icon').className = `bkp__node-icon ${nodeIcon}`;
    document.getElementById(`bkp__sub-${key}`).textContent = stageState.text || '';
  });
  drawStageBodies(wizard, states);
  STAGES.forEach(([key]) => {
    if (showsJobSteps(states, key)) {
      drawSteps(wizard, key);
    }
  });
  updateChoices(wizard);
  drawLog(wizard);
  const refreshText = document.getElementById(REFRESH_BUTTON.textId);
  const refreshSpinner = document.getElementById(REFRESH_BUTTON.spinnerId);
  showSpinner(refreshText, refreshSpinner, flow.refreshing);
}

// true when a stage is working on a job's phase, so its body lists the phase's steps (the
// attach stage works on a drive scan, which has none)
function showsJobSteps(states, stageKey) {
  return states[stageKey].state === 'working' && STAGE_PHASES[stageKey] !== undefined;
}

// Draws the stages' bodies (chips, check button, passphrase, Start, steps, Eject now),
// again only when a stage's state changes, so a passphrase being typed or a switch being
// dragged stays.
function drawStageBodies(wizard, states) {
  const attachedCount = wizard.flow.poolsIn('attached').length;
  const stateKeys = STAGES.map(([key]) => {
    let openFlag = 0;
    if (states[key].open) {
      openFlag = 1;
    }
    return `${key}:${states[key].state}:${openFlag}`;
  });
  const controlsKey = `${stateKeys.join(',')}:${wizard.backupType}:${attachedCount}`;
  if (controlsKey === wizard.drawnControls) {
    return;
  }
  wizard.drawnControls = controlsKey;
  dropEjectSwitch(wizard);
  STAGES.forEach(([key]) => {
    document.getElementById(`bkp__body-${key}`).textContent = '';
  });
  drawTypeChoice();
  const attachOpen = states.attach.state === 'open' || states.attach.open === true;
  if (attachOpen) {
    drawAttach();
  }
  const unlockOpen = states.unlock.state === 'open' || states.unlock.open === true;
  if (unlockOpen) {
    drawUnlock(wizard);
  }
  const backupOpen = states.backup.state === 'open' || (states.backup.open && states.backup.state !== 'working');
  if (backupOpen) {
    drawBackupChoice(wizard);
  }
  STAGES.forEach(([key]) => {
    if (showsJobSteps(states, key)) {
      drawWorking(key);
    }
  });
  if (states.eject.state === 'open') {
    document.getElementById('bkp__body-eject').innerHTML = gridButtonTemplate({
      buttonId: 'bkp__eject', icon: 'fad fa-eject', label: 'Eject now', buttonClass: 'bkp__go'
    });
  }
}

// stage 1's chips: External drives and VMs/LXCs
function drawTypeChoice() {
  const chips = BACKUP_TYPES.map(([type, name, icon]) => gridButtonTemplate({
    buttonId: `bkp__type-${type}`, icon: icon, label: name, buttonClass: 'bkp__chip'
  })).join('');
  document.getElementById('bkp__body-type').innerHTML = `<div class="bkp__chips">${chips}</div>`;
}

// the attach stage's prompt and its check button, which moves on to Unlock drives
function drawAttach() {
  document.getElementById('bkp__body-attach').innerHTML = `
    <div class="bkp__row">
      <span class="bkp__prompt bkp__small">Connect the backup drive(s), then continue</span>
      ${gridButtonTemplate({ buttonId: 'bkp__attach', icon: 'fas fa-check', buttonClass: 'bkp__go bkp__go--icon' })}
    </div>`;
}

// the unlock stage's passphrase box and button; Enter sends it too
function drawUnlock(wizard) {
  const body = document.getElementById('bkp__body-unlock');
  body.innerHTML = `
    <div class="bkp__row">
      ${textBoxTemplate({ boxId: 'bkp__pass', type: 'password', placeholder: 'Passphrase', boxClass: 'bkp__pass' })}
      ${gridButtonTemplate({ buttonId: 'bkp__unlock', icon: 'fad fa-lock-open', buttonClass: 'bkp__go bkp__go--icon' })}
    </div>`;
  document.getElementById('bkp__pass').addEventListener('keyup', keyEvent => {
    if (keyEvent.key === 'Enter') {
      unlockDrives(wizard);
    }
  });
}

// The backup stage's controls: for drives, the Standard and Checksum chips and the
// Eject when finished switch (on to begin with); then Start.
function drawBackupChoice(wizard) {
  const body = document.getElementById('bkp__body-backup');
  const startButton = gridButtonTemplate({
    buttonId: 'bkp__start', icon: 'fad fa-play', label: 'Start', buttonClass: 'bkp__go'
  });
  if (wizard.backupType !== 'external') {
    body.innerHTML = startButton;
    return;
  }
  const chips = DRIVE_BACKUPS.map(([action, name, icon]) => gridButtonTemplate({
    buttonId: `bkp__drive-${action}`, icon: icon, label: name, buttonClass: 'bkp__chip'
  })).join('');
  body.innerHTML = `
    <div class="bkp__chips">${chips}</div>
    <div class="bkp__switchrow"><span class="bkp__switchlabel bkp__small">Eject when finished</span><span id="bkp__switch"></span></div>
    ${startButton}`;
  wizard.ejectSwitch = new ToggleSwitch(document.getElementById('bkp__switch'), {
    id: 'bkp__eject-toggle',
    command: 'eject',
    checked: wizard.ejectAfter,
    width: '84px',
    height: '36px',
    onToggle: payload => {
      wizard.ejectAfter = payload.cmd === 'ejecton';
    }
  });
  wizard.ejectSwitch.activate();
}

// marks the picked chips; the type can be changed only while no job runs
function updateChoices(wizard) {
  const typeLocked = wizard.flow.stage === 'running';
  BACKUP_TYPES.forEach(([type]) => {
    const chip = document.getElementById(`bkp__type-${type}`);
    const picked = type === wizard.backupType;
    chip.classList.toggle('is-picked', picked);
    chip.disabled = typeLocked && !picked;
  });
  DRIVE_BACKUPS.forEach(([action]) => {
    const chip = document.getElementById(`bkp__drive-${action}`);
    if (chip) {
      chip.classList.toggle('is-picked', action === wizard.driveBackup);
    }
  });
}

// a working stage's body: its step list and detail line, and Cancel for a backup
function drawWorking(stageKey) {
  let cancelMarkup = '';
  if (stageKey === 'backup') {
    cancelMarkup = gridButtonTemplate({
      buttonId: 'bkp__cancel', icon: 'fad fa-ban', label: 'Cancel', buttonClass: 'bkp__go bkp__go--danger'
    });
  }
  document.getElementById(`bkp__body-${stageKey}`).innerHTML = `
    <div id="bkp__steps-${stageKey}"></div>
    <div class="bkp__total bkp__tiny bkp__muted" id="bkp__total-${stageKey}"></div>
    ${cancelMarkup}`;
}

// a result's colour class: ok green, warn and cancelled yellow, anything else red
function resultClass(result) {
  if (result === 'ok') {
    return 'bkp__good';
  }
  if (result === 'warn' || result === 'cancelled') {
    return 'bkp__warn';
  }
  return 'bkp__bad';
}

// One step's row: a spinner or its result's icon, its name, and its percent while it
// runs or, on the unlock and eject stages, the drive's outcome once it ends.
function stepRow(phase, stageKey, step) {
  let icon = '<span class="btn-spinner bkp__spinner"></span>';
  if (step.result) {
    icon = `<i class="${STEP_RESULT_ICONS[step.result]} ${resultClass(step.result)}"></i>`;
  }
  const row = makeElement('div', 'bkp__step bkp__small',
    `${icon}<span class="bkp__step-name"></span><span class="bkp__step-pct bkp__tiny"></span>`);
  const percentCell = row.querySelector('.bkp__step-pct');
  row.querySelector('.bkp__step-name').textContent = stepLabel(step);
  const outcome = phase.pools[step.group];
  if (step.result === null && step.percent !== null) {
    percentCell.textContent = `${step.percent}%`;
  } else if (step.result !== null && stageKey !== 'backup' && outcome) {
    percentCell.textContent = POOL_OUTCOME_TEXT[outcome] || outcome;
    let outcomeClass = 'bkp__bad';
    if (POOL_OUTCOME_GOOD.includes(outcome)) {
      outcomeClass = 'bkp__good';
    }
    percentCell.classList.add(outcomeClass);
  }
  return row;
}

// Draws a working stage's step rows and the transfer detail under them. The body (and
// its Cancel) is drawn by drawStageBodies(); only the rows are drawn again here.
function drawSteps(wizard, stageKey) {
  const flow = wizard.flow;
  const stepList = document.getElementById(`bkp__steps-${stageKey}`);
  if (!stepList) {
    return;
  }
  // a job asked for and not begun has no steps yet; the model still holds the one before
  const phase = jobPhase(flow.job, STAGE_PHASES[stageKey]);
  let steps = [];
  if (phase !== null && flow.pending === null) {
    steps = phase.steps.filter(step => step);
  }
  stepList.replaceChildren(...steps.map(step => stepRow(phase, stageKey, step)));
  document.getElementById(`bkp__total-${stageKey}`).textContent = flow.progressDetail;
  const cancelButton = document.getElementById('bkp__cancel');
  if (cancelButton) {
    cancelButton.disabled = !flow.canCancel;
  }
}

// The job's log under the line while Log is on, kept on its newest line unless
// scrolled up.
function drawLog(wizard) {
  const logSlot = document.getElementById('bkp__logslot');
  if (!wizard.showLog) {
    logSlot.textContent = '';
    return;
  }
  let logBox = document.getElementById('bkp__logbox');
  if (!logBox) {
    logSlot.innerHTML = `<div class="popup-ctn bkp__logwrap"><textarea readonly spellcheck="false" class="pico-logbox bkp__logbox scroll-bar-style" id="bkp__logbox"></textarea></div>`;
    logBox = document.getElementById('bkp__logbox');
  }
  const scrolledTo = logBox.scrollTop + logBox.clientHeight;
  const atEnd = scrolledTo >= logBox.scrollHeight - 4;
  logBox.value = wizard.flow.logText;
  if (atEnd) {
    logBox.scrollTop = logBox.scrollHeight;
  }
}

//// actions ////

// sends the passphrase
function unlockDrives(wizard) {
  const passBox = document.getElementById('bkp__pass');
  if (!passBox || passBox.value === '') {
    return;
  }
  const passphrase = passBox.value;
  passBox.value = '';
  wizard.flow.acknowledge();
  wizard.flow.unlock(passphrase);
}

// starts the backup picked: a VM backup, or the drive backup with the eject switch
function startBackup(wizard) {
  let backupAction = wizard.driveBackup;
  if (wizard.backupType === 'vms') {
    backupAction = 'vms';
  }
  wizard.lastBackupDone = null;
  wizard.lastEjectDone = null;
  wizard.flow.acknowledge();
  wizard.flow.backup(backupAction, wizard.ejectAfter);
}

// takes down the eject switch with its stage's body
function dropEjectSwitch(wizard) {
  if (wizard.ejectSwitch) {
    wizard.ejectSwitch.destroy();
    wizard.ejectSwitch = null;
  }
}

// One handler for the window's buttons.
function pressButton(wizard, button) {
  const flow = wizard.flow;
  if (button.id.startsWith('bkp__type-')) {
    // a finished job's result gives way to the new pick, on the line and on the ring
    if (flow.stage === 'finished') {
      flow.acknowledge();
    }
    wizard.ringClearedStarted = flow.job.started;
    wizard.backupType = button.id.replace('bkp__type-', '');
    drawWizard(wizard);
    return;
  }
  if (button.id.startsWith('bkp__drive-')) {
    wizard.driveBackup = button.id.replace('bkp__drive-', '');
    updateChoices(wizard);
    return;
  }
  switch (button.id) {
    case 'bkp__close':
      closeBackupWizard();
      break;
    case 'bkp__log':
      wizard.showLog = !wizard.showLog;
      drawLog(wizard);
      break;
    case 'bkp__refresh':
      flow.refresh();
      break;
    case 'bkp__attach':
      // moves on to Unlock drives once the host has found the drive just connected
      wizard.attachChecked = true;
      flow.rescanDrives();
      break;
    case 'bkp__unlock':
      unlockDrives(wizard);
      break;
    case 'bkp__start':
      startBackup(wizard);
      break;
    case 'bkp__cancel':
      flow.cancel();
      break;
    case 'bkp__eject':
      flow.acknowledge();
      flow.eject();
      break;
  }
}
