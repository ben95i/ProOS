// Automate Website - JavaScript Frontend
// by Ben Provenzano III

// global variables //
const MAIN_SITE = "Automate";
const ERR_RESP = "500 : Internal Server Error"
const BKM_INACTIVE = 0;
const BKM_OPEN_MODE = 1;
const BKM_EDIT_MODE = 2;
let ledScrollData = ["00ff00", "8x8", "12.0"];
let fileData = [];
let ctlCommand;
let serverCmdData;
let messagePopupTimer;
let dynMenuActive = 0;
let bookmarkState = 0;
let volPopupTimer;
let sysModel;

const PRESET_COLORS = [
  '#FF0000', '#FF7F00', '#FFFF00', '#00FF00', '#00FFFF',
  '#0000FF', '#7F00FF', '#FF00FF', '#FFFFFF', '#000000'
];
let isGridInitialized = false;
const colorParser = document.createElement('div');
let isColorCommandPending = false;
let pendingQueuedColor = null;
let currentHSV = { h: 0, s: 1, v: 1 };
let isDraggingMap = false;

// request rate limiter
function throttle(func, limit) {
  let lastFunc;
  let lastRan;
  return function() {
    const context = this;
    const args = arguments;
    if (!lastRan) {
      func.apply(context, args);
      lastRan = Date.now();
    } else {
      clearTimeout(lastFunc);
      lastFunc = setTimeout(function() {
        if ((Date.now() - lastRan) >= limit) {
          func.apply(context, args);
          lastRan = Date.now();
        }
      }, limit - (Date.now() - lastRan));
    }
  };
}

// runs after DOM finishes loading
window.addEventListener("DOMContentLoaded", () => {
  // on-click actions
  window.addEventListener('click', handleClicks, false);
  // load content
  loadPage();
});

// runs on-each mouse click
function handleClicks(event) {
  // disable click events when in bookmark edit mode
  if (bookmarkState === BKM_EDIT_MODE) {
    if (event.target.className !== "editFav__win") {
       // disable all clicks outside of window
      event.stopPropagation();
    }
    return;
  }
  // don't hide menus when clicking these elements
  if (event.target.classList.contains('button') || // button clicks
      event.target.classList.contains('button__text') || // button text clicks
      event.target.classList.contains('nohide__click') || // no-hide clicks
      event.target.classList.contains('bookmarked__item') || // bookmark menu clicks
      event.target.classList.contains('chkbox') || // checkbox clicks
      event.target.classList.contains('fad') || // duotone icon clicks
      event.target.classList.contains('far') || // regular icon clicks
      event.target.classList.contains('fab') || // brand icon clicks
      event.target.classList.contains('fas')) { // solid icon clicks
    return;
  }
  hideDropdowns();
}

function hideDropdowns() {
  // hide all dropdown menus
  classDisplay("dd-content", "none");
  // hide / reset bookmarks menu
  classDisplay("bookmark-buttons", "none");
  const elem = document.getElementById("bookmarks");
  if (elem) {
    elem.classList.remove("bookmark-editmode");
  }
  removeBookmarkSearch();
  closeBookmarkPrompt();
  bookmarkState = BKM_INACTIVE;
  removeDynMenus();
}

// show / hide multiple classes
function classDisplay(_elem, _state) {
  let _itr;
  let _class = document.getElementsByClassName(_elem);
  for (_itr = 0; _itr < _class.length; _itr++) {
    _class[_itr].style.display = _state;
  }
}

function checkElemIsVisibleByID(id){
  let _elmvis = false;
  let _elem = document.getElementById(id);
  if (_elem) {
    var _style = window.getComputedStyle(_elem)
    if (_style.display !== 'none') {
      _elmvis = true;
    }
  }
  return _elmvis;
}

// remove element by ID
function removeWindow(id) {
  let win = document.getElementById(id);
  if (win) { // window exists
    document.body.removeChild(win);
  }
}

/// utilities ///

function hsvToRgb(h, s, v) {
  let r, g, b, i, f, p, q, t;
  h /= 360;
  i = Math.floor(h * 6);
  f = h * 6 - i;
  p = v * (1 - s);
  q = v * (1 - f * s);
  t = v * (1 - (1 - f) * s);
  switch (i % 6) {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    case 5: r = v, g = p, b = q; break;
  }
  return { r: Math.round(r * 255), g: Math.round(g * 255), b: Math.round(b * 255) };
}

function rgbToHsv(r, g, b) {
  r /= 255, g /= 255, b /= 255;
  let max = Math.max(r, g, b), min = Math.min(r, g, b);
  let h, s, v = max;
  let d = max - min;
  s = max === 0 ? 0 : d / max;
  if (max === min) {
    h = 0; 
  } else {
    switch (max) {
      case r: h = (g - b) / d + (g < b ? 6 : 0); break;
      case g: h = (b - r) / d + 2; break;
      case b: h = (r - g) / d + 4; break;
    }
    h /= 6;
  }
  return { h: h * 360, s: s, v: v };
}

function hexToRgb(hex) {
  let bigint = parseInt(hex.replace('#', ''), 16);
  return { r: (bigint >> 16) & 255, g: (bigint >> 8) & 255, b: bigint & 255 };
}

function rgbToHex(r, g, b) {
  return "#" + ((1 << 24) + (r << 16) + (g << 8) + b).toString(16).slice(1).toUpperCase();
}

function isNumeric(value) {
  return /^-?\d+$/.test(value);
}

function mapNumber(num, inMin, inMax, outMin, outMax) {
  return (num - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

///////////

function showPopup(message) {
  let msgtext;
  let win_name = "msg-popup";
  let text_name = "msg-text";
  let msgpopwin = document.getElementById(win_name);
  if (!msgpopwin) { // window does not exist
    // draw window
    msgpopwin = document.createElement("div");
    msgpopwin.classList.add("popup-alert");
    msgpopwin.id = win_name;
    // volume text box
    msgtext = document.createElement("div");
    msgtext.id = text_name;
    msgpopwin.appendChild(msgtext);
    // add to page
    document.body.appendChild(msgpopwin);
  } else {
    // select elements in existing window
    msgtext = document.getElementById(text_name);
  }
  // set error text
  msgtext.innerHTML = message;
  // reset hide window timeout
  clearTimeout(messagePopupTimer);
  // hide window after delay
  messagePopupTimer = setTimeout(() => {
    if (msgpopwin) { // window exists
      // fade out window
      msgpopwin.classList.add('fade-out');
      // wait for the transition to end before removing window
      msgpopwin.addEventListener("animationend", function (event) {
        if (event && msgpopwin) {
          document.body.removeChild(msgpopwin);
        }
      });
    }
  }, 4000); // hide delay in (ms)
}

// open URL in new tab
function GoToExtPage(_path) {
  let url = "https://" + _path;   
  window.open(url, "_blank");
}

// back to home page 
function GoToHomePage() {
  if (sysModel === MAIN_SITE) {
    hidePages();
    loadPage();
  } else {
    window.location = 'https://' + MAIN_SITE + '.home';   
  }
}

function hidePages() {
  classDisplay('pi-grid', 'none'); 
  classDisplay('ledpi-grid', 'none'); 
  classDisplay('server-grid', 'none');
}

// runs on page load
function loadPage() {
  // read device type
  sysModel = deviceType(); 
  if (sysModel === MAIN_SITE) {
    // load control menu saved state
    let _mode = localStorage.getItem("ctls-mode")
    if (_mode === null || _mode === undefined || _mode === "") { 
      _mode = 'lr'; 
    } // living room
    ctlsMenu(_mode);
    // title-bar room state
    showRoomState(5);
    // server home page
    classDisplay('server-grid', 'block');
  } else if (sysModel === 'Pi') {
    classDisplay('pi-grid', 'block');
  } else if (sysModel === 'LEDpi') {
    classDisplay('ledpi-grid', 'block');
  }
  // set theme
  let currentTheme;
  let _theme = localStorage.getItem("main-color")
  if (_theme === null || _theme === undefined || _theme === "") {
    currentTheme = "#1f2051"; // dark blue
  } else {
    currentTheme = localStorage.getItem("main-color");
  }  
  setTheme(currentTheme);
  animatedBackground();
}

function setTheme(newTheme) {
  let body = document.getElementsByTagName("html")[0];
  body.style.setProperty('--main-color', newTheme);
  localStorage.setItem("main-color", newTheme);
}

function showRoomState(duration) {
  let lightIcon = document.getElementById("trafficLightIcon");
  let statusMsg = document.getElementById("statusMessage");
  let container = document.getElementById('statusDropdownContainer');
  container.classList.remove('status-loading');
  let room = null; let cmd = null;
  if (ctlCommand == 'lr' || ctlCommand == 'subs') {
    room = "living room"; cmd = 'lrstatus';
  }
  if (ctlCommand == 'br') {
    room = "bedroom"; cmd = 'brstatus';
  }
  if (cmd == null) return;
  lightIcon.classList.add("status-pending");
  statusMsg.textContent = "Scanning devices...";
  sendCmd('main', cmd, '').then((resp) => {
    let success = false;
    if (resp == "1") {
      lightIcon.classList.remove("status-error");
      lightIcon.classList.remove("status-off");
      lightIcon.classList.add("status-on");
      statusMsg.textContent = "Devices in the " + room + " are on";
      success = true;
    } else if (resp == "0") { 
      lightIcon.classList.remove("status-error");
      lightIcon.classList.remove("status-on");
      lightIcon.classList.add("status-off");
      statusMsg.textContent = "Devices in the " + room + " are off";
      success = true;
    } else {
      lightIcon.classList.remove("status-off");
      lightIcon.classList.remove("status-on");
      lightIcon.classList.add("status-error");
      statusMsg.textContent = "Error checking devices!";
    }
    lightIcon.classList.remove("status-pending");
    if (success) {
      container.style.setProperty('--spin-duration', `${duration}s`);    
      void container.offsetWidth; 
      requestAnimationFrame(() => {
        container.classList.add('status-loading');
        setTimeout(() => {
          showRoomState(duration);
        }, duration * 1000);
      });
    }
  });
}

/// server popup window ///

async function serverSend() {
  if (serverCmdData === null) {
    // load log data
    document.getElementById("logTextBox").value = "select an option.";
  } else {
    // send command
    sendCmd('main','server',serverCmdData);
    // display command sent
    document.getElementById("logTextBox").value += "\n" + serverCmdData + " command sent.";
    // scroll to bottom of page
    let txtArea = document.getElementById("logTextBox");
    txtArea.scrollTop = txtArea.scrollHeight;
  }
  sendBtnAlert("off");
  serverCmdData = null;
}

function sendBtnAlert(state) {
  let _class = "alert_btn";
  let _elem = "sendButton";
  if (state === 'on') {
    addClassToElem('add', _elem, _class);
  }
  if (state === 'off') {
    addClassToElem('remove', _elem, _class);
  }
}

function clearPendingCmd() {
  sendBtnAlert("off");
  serverCmdData = null;
}

function openServerOptions(){
  closeServerOptions();
  classDisplay("svropt__prompt", "block");
  classDisplay("svropt__main", "block");
}

function openRegionsOptions(){
  closeServerOptions();
  classDisplay("svropt__prompt", "block");
  classDisplay("svropt__regions", "block");
}

function openBackupOptions(){
  closeServerOptions();
  classDisplay("svropt__prompt", "block");
  classDisplay("svropt__backup", "block");
}

function closeServerOptions(){
  hideDropdowns();
  clearPendingCmd();
  classDisplay("svropt__prompt", "none");
  classDisplay("svropt__main", "none");
  classDisplay("svropt__regions", "none");
  classDisplay("svropt__backup", "none");
}

/////// LEDScroll ///////

function setTextScrollTitle(){
  let title = document.getElementById('textScrollTitle');
  const color = ledScrollData[0];
  const font = ledScrollData[1];
  const speed = ledScrollData[2];
  title.style.color = '#' + color;
  title.textContent = "Textscroll " + font + ' ' + speed + 'x';
}

function clearTextScrollBox(){
  let textbox = document.getElementById('textScrollBox');
  textbox.placeholder = "Enter message";
  textbox.value = '';
}

function openTextScroll(){
  classDisplay("textscroll__prompt", "block");
  clearTextScrollBox();
  setTextScrollTitle();
}

function closeTextScroll(){
  classDisplay("textscroll__prompt", "none");
  clearTextScrollBox();
  hideDropdowns();
}

function clearLEDsMessage(){
  clearTextScrollBox();
  sendCmd('leds', 'stop', '');
}

function setTextScrollColor(color){
  ledScrollData[0] = color;
  setTextScrollTitle();
}

function setTextScrollFont(font){
  ledScrollData[1] = font;
  setTextScrollTitle();
}

function setTextScrollSpeed(speed){
  ledScrollData[2] = speed;
  setTextScrollTitle();
}

function sendTextScroll(){
  let textbox = document.getElementById('textScrollBox');
  const message = cleanString(textbox.value.replaceAll("|", "-"));
  if (message == null || message == "") {
    textbox.placeholder = "Message cannot be empty";
    return;
  }
  textbox.placeholder = "Sending message..";
  textbox.value = '';
  const color = ledScrollData[0];
  const font = ledScrollData[1];
  const speed = ledScrollData[2];
  const commands = `${message}|${color}|${font}|${speed}`;
  const encoded = base64URLSafeEncode(commands);
  sendCmd('leds', 'textscroll', encoded).then((data) => {
    if (data == ERR_RESP) {
      textbox.placeholder = "Error sending message!"; 
    } else {
      textbox.placeholder = "Message sent";
    }
    setTimeout(function(){
      textbox.value = message;
    }, 1000); // in ms
  });
}

/////////////////////////

async function aboutPrompt(){
  const _winid = 'about__prompt';
  // only allow one-instance of the window
  if (document.getElementById(_winid)) {
    return;
  }
  let aboutprompt = document.createElement("div"); 
  aboutprompt.id = _winid;
  aboutprompt.classList.add("prompt_win");
  // title
  let abouttext = document.createElement("div");
  abouttext.className = "about__title";
  abouttext.innerHTML = sysModel;
  aboutprompt.appendChild(abouttext); 
  // logo
  let img = document.createElement("img");
  img.src = "img/automate.png";
  img.id = "about__img";
  aboutprompt.appendChild(img);
  // version details
  let aboutdets2 = document.createElement("div"); 
  aboutdets2.innerHTML = "by Ben Provenzano III";
  aboutdets2.className = "about__text";
  aboutprompt.appendChild(aboutdets2);
  let aboutdets1 = document.createElement("div");
  const curDate = new Date();
  const curYear = curDate.getFullYear();
  const curMonth = curDate.getMonth() + 1;
  const curQtr = Math.ceil(curMonth / 3); // round up to the nearest integer
  aboutdets1.innerHTML = curYear + '.' + curQtr + " Release";
  aboutdets1.className = "about__text";
  aboutprompt.appendChild(aboutdets1);
  // cancel button
  let aboutcancelb = document.createElement("button");
  aboutcancelb.type = "button"; 
  aboutcancelb.classList.add("button");
  aboutcancelb.innerHTML = "Close";
  aboutcancelb.id = "about__btn";
  aboutprompt.appendChild(aboutcancelb); //append cancel-button
  document.body.appendChild(aboutprompt); //append the password-prompt so it gets visible
  new Promise(function() {
    aboutprompt.addEventListener('click', function handleButtonClicks(btn_evnt) { //lets handle the buttons
      if (btn_evnt.target.tagName !== 'BUTTON') { return; } //nothing to do - user clicked somewhere else
      aboutprompt.removeEventListener('click', handleButtonClicks); //removes eventhandler on cancel or ok
      document.body.removeChild(aboutprompt);  //as we are done clean up by removing the password-prompt
    });
  });   
}

/// Temperature & Humidity ///

async function showTempHumidity(){
  const _winid = 'temp__prompt';
  // only allow one-instance of the window
  if (document.getElementById(_winid)) {
    return;
  }
  // create window
  let tempprompt = document.createElement("div"); 
  tempprompt.id = _winid;
  tempprompt.classList.add("prompt_win");
  // top window text
  let temptext = document.createElement("div");
  temptext.innerHTML = 'Temperature/Humidity';
  temptext.id = 'temp_top_text';
  tempprompt.appendChild(temptext);
  // thermometer container
  let tempcon = document.createElement("div");
  tempcon.id = 'temp_therm_grid';
  // temperature thermometer 
  let tmeter = document.createElement("div"); 
  tmeter.className = "thermometer";
  let tdisplay = document.createElement("div"); 
  tdisplay.className = "temperature";
  tdisplay.id = "thermo__1";
  tdisplay.dataset.value = "--°";
  tmeter.appendChild(tdisplay); 
  tempcon.appendChild(tmeter); 
  // humidity thermometer 
  let hmeter = document.createElement("div"); 
  hmeter.className = "thermometer";
  let hdisplay = document.createElement("div"); 
  hdisplay.className = "temperature";
  hdisplay.id = "thermo__2";
  hdisplay.dataset.value = "--%";
  hmeter.appendChild(hdisplay);
  tempcon.appendChild(hmeter); 
  // add thermo container to window
  tempprompt.appendChild(tempcon);
  // buttons container
  let btnscon = document.createElement("div");
  // cancel button
  let tempcancelb = document.createElement("button");
  tempcancelb.classList.add("button");
  tempcancelb.classList.add("temp__btn");
  tempcancelb.classList.add("fas");
  tempcancelb.classList.add("fa-times");
  tempcancelb.type = "button";
  btnscon.appendChild(tempcancelb);
  // refresh button
  let temprefreshb = document.createElement("button");
  temprefreshb.classList.add("button");
  temprefreshb.classList.add("temp__btn");
  temprefreshb.classList.add("fad");
  temprefreshb.classList.add("fa-sync");
  temprefreshb.type = "button";
  btnscon.appendChild(temprefreshb);
  // add buttons container to window
  tempprompt.appendChild(btnscon);
  // add window to DOM
  document.body.appendChild(tempprompt);
  // call API for data
  getTemperatureData();
  // button actions
  new Promise(function() {
    tempprompt.addEventListener('click', function handleButtonClicks(btn_evnt) {
      if (btn_evnt.target.tagName !== 'BUTTON') { return; } 
        // close button
        if (btn_evnt.target === tempcancelb) {
          tempprompt.removeEventListener('click', handleButtonClicks); 
          document.body.removeChild(tempprompt);  
        }
        // refresh button
        if (btn_evnt.target === temprefreshb) {
          getTemperatureData();
        }
    });
  });
}

function getTemperatureData() {
  sendCmd('main', 'brxmit', 'roomth').then((data) => {
    const resp = cleanString(data);
    // extract numerics
    const resp_arr = resp.split("~");
    let temp_elm = document.getElementById("thermo__1");
    let humd_elm = document.getElementById("thermo__2");
    if (resp_arr.length == 2) {
      // valid response
      pushTempDataToThermos(resp_arr, temp_elm, humd_elm);
    } else {
      pushTempErrorThermos(temp_elm, humd_elm);
    }
  });
}

function pushTempDataToThermos(resp_arr, temp_elm, humd_elm) {
  // thermometer limits
  const minTemp = 25;
  const maxTemp = 100;
  const minHumidity = 5;
  const maxHumitidy = 100;
  // verify elements exist
  if (!(temp_elm && humd_elm)) {
    return;
  }
  // validate response
  if (resp_arr.length == 2) {
    let tvalue = resp_arr[0];
    let hvalue = resp_arr[1];
    // set temperature
    temp_elm.dataset.value = tvalue + "°F";
    if (tvalue >= maxTemp) { tvalue = maxTemp; }
    if (tvalue <= minTemp) { tvalue = minTemp; }
    temp_elm.style.height = (tvalue - minTemp) / (maxTemp - minTemp) * 100 + "%";
    // set humidity
    humd_elm.dataset.value = hvalue + "%";
    if (hvalue >= maxHumitidy) { hvalue = maxHumitidy; }
    if (hvalue <= minHumidity) { hvalue = minHumidity; }
    humd_elm.style.height = (hvalue - minHumidity) / (maxHumitidy - minHumidity) * 100 + "%";
  } else {
    pushTempErrorThermos(temp_elm, humd_elm);
  }
}

function pushTempErrorThermos(temp_elm, humd_elm) {
  // verify elements exist
  if (!(temp_elm && humd_elm)) {
    return;
  }
  temp_elm.style.height = "0%";
  temp_elm.dataset.value = "--";
  humd_elm.style.height = "0%";
  humd_elm.dataset.value = "--";
}

/// END - Temperature & Humidity ///

async function showPiWiFiPrompt() {
  const winid = "pinet__prompt";
  // only allow one-instance of the window
  if (document.getElementById(winid)) {
    return;
  }
  hideDropdowns();
  let result = await piWiFiPrompt(winid);
  if (result !== null) {  
    if (result !== '') {  
      sendCmd('main','confwpa',result);
      document.getElementById("logTextBox").value = "Wi-Fi configuration updated, select client mode to apply changes.";
    }
  }
}

async function piWiFiPrompt(_winid){
  let pinetprompt = document.createElement("div"); 
  pinetprompt.id = _winid;
  pinetprompt.classList.add("prompt_win");
  // SSID text
  let pinettext1 = document.createElement("div"); 
  pinettext1.innerHTML = "Network (SSID):"; 
  pinetprompt.appendChild(pinettext1); 
  // SSID box 
  let pinetssidbox = document.createElement("input"); 
  pinetssidbox.id = "pinet__ssidbox"; 
  pinetprompt.appendChild(pinetssidbox); 
  // password text
  let pinettext2 = document.createElement("div"); 
  pinettext2.innerHTML = "Password:"; 
  pinetprompt.appendChild(pinettext2);   
  // password box
  let pinetpassbox = document.createElement("input"); 
  pinetpassbox.id = "pinet__passbox"; 
  pinetpassbox.type="password";
  pinetprompt.appendChild(pinetpassbox); 
  // save button
  let pinetokbutton = document.createElement("button"); 
  pinetokbutton.innerHTML = "Save";
  pinetokbutton.className ="button pinet__btn"; 
  pinetokbutton.type="button"; 
  // cancel button
  let pinetcancelb = document.createElement("button"); 
  pinetcancelb.innerHTML = "Cancel";
  pinetcancelb.className ="button pinet__btn"; 
  pinetcancelb.type="button"; 
  // create window
  pinetprompt.appendChild(pinetcancelb); 
  pinetprompt.appendChild(pinetokbutton); 
  document.body.appendChild(pinetprompt); 
  // focus on SSID text box
  pinetssidbox.focus();
  // control logic
  return new Promise(function(resolve) {
  	function cancelWiFi(){
  		pinetssidbox.value = "";
  		pinetpassbox.value = "";
        document.body.removeChild(pinetprompt);
    }
  	function sendWiFiData(){
	    if (pinetssidbox.value === "") {
	      pinettext1.innerHTML = "enter SSID!";
	    } else {
	      if (pinetpassbox.value === "") {
	        pinettext1.innerHTML = "enter password!";
	      } else {
	      	// concat and convert to base64
          let _wifidata = base64URLSafeEncode(pinetssidbox.value + "|$|" + pinetpassbox.value);
	        resolve(_wifidata);
	        cancelWiFi();
	      }
	    }
    }
    // button clicks
    pinetprompt.addEventListener('click', function handleButtonClicks(e) { 
      if (e.target.tagName !== 'BUTTON') { return; }
        if (e.target === pinetokbutton) {
          sendWiFiData();
        }
        if (e.target === pinetcancelb) {
          cancelWiFi();
        }        
    });
    // key focused on SSID box
    pinetssidbox.addEventListener('keyup', function handleSSID(ssid_evnt){ 
        if(ssid_evnt.key === 'Enter'){ //if user enters "enter"-key on password-field
          pinetpassbox.focus(); // focus on password field
        } else if (ssid_evnt.key === 'Escape'){ //user enters "Escape" on password-field
          cancelWiFi();
        }
    });
    // key focused on password box
    pinetpassbox.addEventListener('keyup', function handlePass(pass_evnt){ 
        if(pass_evnt.key === 'Enter'){ //if user enters "enter"-key on password-field
          sendWiFiData();
        } else if (pass_evnt.key === 'Escape'){ //user enters "Escape" on password-field
          cancelWiFi();
        }
    });
  }); 
}

async function wifiPrompt(){
  const _winid = "wifi__prompt";
  // only allow one-instance of the window
  if (document.getElementById(_winid)) {
    return;
  }
  let wifiprompt = document.createElement("div");
  wifiprompt.id = _winid;
  wifiprompt.classList.add("prompt_win");
  let wifitext = document.createElement("div");
  wifitext.innerHTML = "Scan for WiFi Access"; 
  wifiprompt.appendChild(wifitext);
  let img = document.createElement("img");
  img.src = "img/wifi.png";
  img.id = "wifi__img";
  wifiprompt.appendChild(img);
  let wifipwd = document.createElement("div");
  wifipwd.id = "wifi__pwdtxt";
  wifipwd.innerHTML = atob(SSIDpwd());
  wifiprompt.appendChild(wifipwd);
  let wificancelb = document.createElement("button");
  wificancelb.innerHTML = "Close";
  wificancelb.className = "button"; 
  wificancelb.type = "button"; 
  wificancelb.id = "wifipmt__btn";
  wifiprompt.appendChild(wificancelb);
  document.body.appendChild(wifiprompt);
  new Promise(function() {
    wifiprompt.addEventListener('click', function handleButtonClicks(btn_evnt) {
      if (btn_evnt.target.tagName !== 'BUTTON') { return; }
        wifiprompt.removeEventListener('click', handleButtonClicks);
        document.body.removeChild(wifiprompt);
    });
  });   
}

async function getPassword(_type){
  let result;
  try{
    hideDropdowns();
    result = await passwordPrompt();
    if (result !== null) {  
      if (result !== '') {
        savePOST(_type, [result]);
      }
    }
    result = "";
  } catch(e){
    result = "";
  }
}

function passwordPrompt(){
  const _winid = "pass__prompt_win";
  // only allow one-instance of the window
  if (document.getElementById(_winid)) {
    return;
  }
  let pwprompt = document.createElement("div"); //creates the div to be used as a prompt
  pwprompt.id = _winid; //gives the prompt an id
  pwprompt.classList.add("prompt_win");
  let pwtextdiv = document.createElement("div"); //create the div for the password-text
  pwtextdiv.innerHTML = "Enter password:"; //put inside the text
  pwprompt.appendChild(pwtextdiv); //append the text-div to the password-prompt
  let pwinput = document.createElement("input"); //creates the password-input
  pwinput.id = "pass__textbox"; //give it some id - not really used in this example...
  pwinput.type="password"; // makes the input of type password to not show plain-text
  pwprompt.appendChild(pwinput); //append it to password-prompt
  // buttons 
  let pwbtndiv = document.createElement("div");
  pwbtndiv.id = "pass__prmbtnctr";
  let pwokbutton = document.createElement("button"); //the ok button
  pwokbutton.innerHTML = "Send";
  pwokbutton.className ="button pass__prompt_btn"; 
  pwokbutton.type="button"; 
  let pwcancelb = document.createElement("button"); //the cancel-button
  pwcancelb.innerHTML = "Cancel";
  pwcancelb.className = "button pass__prompt_btn"; 
  pwcancelb.type = "button"; 
  pwbtndiv.appendChild(pwcancelb); //append cancel-button first
  pwbtndiv.appendChild(pwokbutton); //append the ok-button
  pwprompt.appendChild(pwbtndiv); // append button center div to prompt div
  //append the password-prompt so it gets visible
  document.body.appendChild(pwprompt); 
  pwinput.focus(); //focus on the password-input-field so user does not need to click
  return new Promise(function(resolve, reject) {
      pwprompt.addEventListener('click', function handleButtonClicks(click_evnt) { //lets handle the buttons
        if (click_evnt.target.tagName !== 'BUTTON') { return; } //nothing to do - user clicked somewhere else
        pwprompt.removeEventListener('click', handleButtonClicks); //removes eventhandler on cancel or ok
        if (click_evnt.target === pwokbutton) { //click on ok-button
          resolve(pwinput.value); //return the value of the password
        } else {
          reject(new Error('User cancelled')); //return an error
        }
        document.body.removeChild(pwprompt);  //as we are done clean up by removing the password-prompt
      });
      pwinput.addEventListener('keyup',function handleEnter(up_events){ //users dont like to click on buttons
          if(up_events.key === 'Enter') { //if user enters "enter"-key on password-field
              resolve(pwinput.value); //return password-value
              document.body.removeChild(pwprompt); //clean up by removing the password-prompt
          } else if (up_events.key === 'Escape') { //user enters "Escape" on password-field
              document.body.removeChild(pwprompt); //clean up the password-prompt
              reject(new Error("User cancelled")); //return an error
          }
      });
  }); 
}

/// Color Prompt ///

const throttledColorSend = throttle((strippedHex) => {
  if (isColorCommandPending) return; 
  isColorCommandPending = true;
  sendCmd('leds', 'hex', strippedHex)
    .catch(err => {
      console.error("Failed to send color command:", err);
    })
    .finally(() => {
      isColorCommandPending = false;
    });
}, 50);

function dispatchColorCommand(hex) {
  let strippedHex = hex.replace('#', '');
  throttledColorSend(strippedHex);
}

function updateUIFromHSV(triggerSend = true) {
  let rgb = hsvToRgb(currentHSV.h, currentHSV.s, currentHSV.v);
  let hex = rgbToHex(rgb.r, rgb.g, rgb.b);
  document.getElementById('hex-color').value = hex;
  colorParser.style.color = hex;
  document.querySelectorAll('.color-swatch').forEach(swatch => {
    if (swatch.style.backgroundColor === colorParser.style.color) {
      swatch.classList.add('active-swatch');
    } else {
      swatch.classList.remove('active-swatch');
    }
  });
  if (triggerSend) dispatchColorCommand(hex);
}

function updateStateFromHex(hex, updateMapUI = true, triggerSend = true) {
  let rgb = hexToRgb(hex);
  currentHSV = rgbToHsv(rgb.r, rgb.g, rgb.b);
  if (updateMapUI) {
    document.getElementById('slider-hue').value = currentHSV.h;
    document.getElementById('color-map-bg').style.background = `hsl(${currentHSV.h}, 100%, 50%)`;
    document.getElementById('color-pointer').style.left = `${currentHSV.s * 100}%`;
    document.getElementById('color-pointer').style.top = `${(1 - currentHSV.v) * 100}%`;
  }
  document.getElementById('hex-color').value = hex.toUpperCase();
  updateUIFromHSV(triggerSend);
}

function handleHexInput(e) {
  let val = e.target.value;
  if (!val.startsWith('#')) val = '#' + val;
  if (/^#[0-9A-F]{6}$/i.test(val)) {
    updateStateFromHex(val, true, true);
  }
}

function handleHueInput(e) {
  currentHSV.h = parseFloat(e.target.value);
  document.getElementById('color-map-bg').style.background = `hsl(${currentHSV.h}, 100%, 50%)`;
  updateUIFromHSV(true);
}

// Map Dragging
function startMapDrag(e) {
  isDraggingMap = true;
  updateMapPointer(e);
  window.addEventListener('pointermove', updateMapPointer);
  window.addEventListener('pointerup', stopMapDrag);
  window.addEventListener('pointercancel', stopMapDrag);
}

function updateMapPointer(e) {
  if (!isDraggingMap) return;
  const mapRect = document.getElementById('color-map').getBoundingClientRect();
  let x = Math.max(0, Math.min(e.clientX - mapRect.left, mapRect.width));
  let y = Math.max(0, Math.min(e.clientY - mapRect.top, mapRect.height));
  document.getElementById('color-pointer').style.left = `${(x / mapRect.width) * 100}%`;
  document.getElementById('color-pointer').style.top = `${(y / mapRect.height) * 100}%`;
  currentHSV.s = x / mapRect.width;
  currentHSV.v = 1 - (y / mapRect.height);
  updateUIFromHSV(true);
}

function stopMapDrag() {
  isDraggingMap = false;
  window.removeEventListener('pointermove', updateMapPointer);
  window.removeEventListener('pointerup', stopMapDrag);
  window.removeEventListener('pointercancel', stopMapDrag);
}

function initColorGrid() {
  if (isGridInitialized) return;
  const grid = document.getElementById('preset-color-grid');
  if (!grid) return;
  grid.innerHTML = ''; 
  PRESET_COLORS.forEach(color => {
    const swatch = document.createElement('div');
    swatch.className = 'color-swatch';
    swatch.style.backgroundColor = color;
    swatch.addEventListener('click', () => updateStateFromHex(color, true, true));
    grid.appendChild(swatch);
  });
  isGridInitialized = true;
}

function colorPrompt(initialColor) {
  initColorGrid();
  let formattedColor = String(initialColor);
  if (!formattedColor.startsWith('#')) {
    formattedColor = '#' + formattedColor;
  }
  const pendingLock = isColorCommandPending;
  isColorCommandPending = true; 
  updateStateFromHex(formattedColor, true, false);
  isColorCommandPending = pendingLock; 
  document.getElementById('hex-color').addEventListener('input', handleHexInput);
  document.getElementById('slider-hue').addEventListener('input', handleHueInput);
  document.getElementById('color-map').addEventListener('pointerdown', startMapDrag);
  document.getElementById('color-popup').style.display = 'flex';
}

function closeColorPrompt() {
  document.getElementById('color-popup').style.display = 'none';
  document.getElementById('hex-color').removeEventListener('input', handleHexInput);
  document.getElementById('slider-hue').removeEventListener('input', handleHueInput);
  document.getElementById('color-map').removeEventListener('pointerdown', startMapDrag);
  stopMapDrag();
}

/// END DYNAMIC WINDOWS ///

function relaxSend(_cmd) {
  sendCmd('main', 'relax', _cmd);
}

// volume controls
function sendVol(_cmd) {
  if (ctlCommand == 'lr' ){
    setAmpVolume(_cmd, 'lrxmit', 100); // living room system
  }
  if (ctlCommand == 'subs' ){
    sendCmd('main', 'lrxmit', 'sub' + _cmd); // living room subwoofers
  }  
  if (ctlCommand == 'br' ){
    setAmpVolume(_cmd, 'brpi', 192); // bedroom system
  }
}

function setAmpVolume(_cmd, _device, _maxvol) {
  sendCmd('main', _device, 'vol' + _cmd).then((data) => { // GET request
    let amp_vol = cleanString(data);
    if (isNumeric(amp_vol) === true) {
      // re-map volume data to 0-100%, show volume pop-up
      let display_value;
      if (_maxvol == 100) {
        display_value = Number(amp_vol);
      } else {
        display_value = Number(Math.round(mapNumber(amp_vol, 0, _maxvol, 0, 100)));
      }
      showVolumePopup(display_value); 
    } else {
      if (amp_vol == "") {
        // empty response = power off
        showVolumePopup(-1);
      }
    }
  });
}

function showVolumePopup(vol) {
  let win_name = "vol-popup";
  let text_name = "vol-text";
  let grid_name = "vol-grid";
  let bar_name = "vol-bar";
  let voltext, volgrid, volbar;
  let volpopwin = document.getElementById(win_name);
  if (!volpopwin) { // window does not exist
    // draw window
    volpopwin = document.createElement("div");
    volpopwin.classList.add("popup-alert");
    volpopwin.id = win_name;
    // volume text box
    voltext = document.createElement("div");
    voltext.id = text_name;
    volpopwin.appendChild(voltext);
    // progress bar
    volgrid = document.createElement("div");
    volgrid.classList.add("progress_grid");
    volgrid.id = grid_name;
    volbar = document.createElement("div");
    volbar.classList.add("bar_progress");
    volbar.id = bar_name;
    volgrid.appendChild(volbar);
    volpopwin.appendChild(volgrid);
    // add to page
    document.body.appendChild(volpopwin);
  } else {
    // select elements in existing window
    volbar = document.getElementById(bar_name);
    volgrid = document.getElementById(grid_name);
    voltext = document.getElementById(text_name);
  }
  // set volume display
  switch(vol) {
    case -1:
      volgrid.style.display = "none";
      voltext.innerHTML = "---";
      break;    
    case 0:
      volgrid.style.display = "none";
      voltext.innerHTML = "Mute";
      break;     
    default:
      volgrid.style.display = "block";
      let volstr = vol + "%";
      voltext.innerHTML = volstr;
      volbar.style.width = volstr;
  }
  // reset hide window timeout
  clearTimeout(volPopupTimer);
  // hide window after delay
  volPopupTimer = setTimeout(() => {
    if (volpopwin) { // window exists
      // fade out window
      volpopwin.classList.add('fade-out');
      // wait for the transition to end before removing window
      volpopwin.addEventListener("animationend", function (event) {
        if (event && volpopwin) {
          document.body.removeChild(volpopwin);
        }
      });
    }
  }, 3000); // hide delay in (ms)
}

function roomOnOff(action) {
  if (ctlCommand == 'lr') {
    // living room
    sendCmd('main', 'lr' + action, '');
    return;
  }
  if (ctlCommand == 'br') {
    // bedroom
    sendCmd('main', 'br' + action, '');
    return;
  }
}

function subModeToggle() {
  // toggle subwoofer mode
  if (ctlCommand == 'subs') {
    ctlsMenu('lr');
    return;
  }
  if (ctlCommand != 'subs') {
    ctlsMenu('subs');
    return;
  } 
}

// add / remove a class from a element
function addClassToElem(_action, _elmid, _class) {
  let _elm = document.getElementById(_elmid);
  if (_action === 'add') {
    if (!(_elm.classList.contains(_class))) {
      _elm.classList.add(_class);
    }
  }
  if (_action === 'remove') {
    if (_elm.classList.contains(_class)) {
      _elm.classList.remove(_class);
    } 
  }
}

function subMode(_action) {
  // hide subwoofer mode indicator
  addClassToElem(_action, 'voldwnbtn', 'submode_btn');
  addClassToElem(_action, 'volmutebtn', 'submode_btn');
  addClassToElem(_action, 'volupbtn', 'submode_btn');
  //addClassToElem(_action, 'subwooferbtn', 'submode_btn');
}

// controls menu actions
function ctlsMenu(_mode) {
  ctlCommand = _mode; 
  // living room controls
  if ((_mode === 'lr') || (_mode === 'subs')) {
    // disable bedroom grid
    classDisplay('bedroom-grid', 'none');
    // enable hifi grid
    classDisplay('hifi-grid', 'block');
    if (_mode === 'lr') {
      // hide subwoofer controls 
      subMode('remove');
    } else {
      // subwoofer controls
      subMode('add');
    }
  } 
  // bedroom controls
  if (_mode === 'br') { 
    // disable hifi grid
    classDisplay('hifi-grid', 'none');
    // hide subwoofer controls 
    subMode('remove');
    // enable bedroom grid
    classDisplay('bedroom-grid', 'block');
  }
  // save state 
  localStorage.setItem("ctls-mode", _mode);
}

// toggle dropdown menu's
function showMenu(_menu) {
  let _elem = document.getElementById(_menu);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    hideDropdowns();
    _elem.style.display = 'block';
  }
}

//// Bookmarks Menu ////

function showBookmarks() {
  // hide menu if clicked while open
  if (bookmarkState !== BKM_INACTIVE) {
    hideDropdowns();
    return;
  }
  // draw bookmarks
  showDynMenu('bookmarks');
  // show add / edit buttons
  classDisplay("bookmark-buttons", "block");
  // link open mode
  bookmarkState = BKM_OPEN_MODE;
}

function showBookmarkSearch() {
  const elem = document.getElementById("bookmarks");
  if (elem) {
    // draw text input box
    let searchBox = document.createElement("input"); 
    searchBox.type = "text";
    searchBox.autocorrect = "off";
    searchBox.autocapitalize = "none"; 
    searchBox.placeholder = "Search here...";
    searchBox.classList.add('nohide__click');
    searchBox.id = 'bookmark__search_txt';
    // draw menu container 
    let searchMenuItem = document.createElement('a');
    searchMenuItem.id = "bookmark__search";
    searchMenuItem.classList.add('nohide__click');
    // draw search icon
    let searchIcon = document.createElement("div");
    searchIcon.id = "bookmark__search_icon";
    showBookmarkSearchIcon(searchIcon);
    // add objects to window
    searchMenuItem.appendChild(searchIcon);
    searchMenuItem.appendChild(searchBox);
    elem.appendChild(searchMenuItem);
    // focus keyboard on search text box
    searchBox.focus();
    new Promise(function() {
      // add input listener
      searchMenuItem.addEventListener('input', function searchInputListener(int_evnt) {
        filterBookmarkItems(searchBox.value);
        // cleanup listener
        if (int_evnt.target.tagName !== 'INPUT') { return; }
        searchMenuItem.removeEventListener('click', searchInputListener); 
      });
      // search icon button listener
      searchMenuItem.addEventListener('click', function handleSearchButtonClick(btn_evnt) {
        // reset search
        filterBookmarkItems("");
        searchBox.value = "";
        searchBox.focus();
        // cleanup listener
        if (btn_evnt.target.tagName !== 'BUTTON') { return; }
        searchMenuItem.removeEventListener('click', handleSearchButtonClick);
      });
    });
  }
}

function filterBookmarkItems(termIn) {
  const searchTerm = termIn.toLowerCase();
  const searchIcon = document.getElementById("bookmark__search_icon")
  if (searchTerm === "") {
    showBookmarkSearchIcon(searchIcon);
  } else {
    showCancelSearchIcon(searchIcon);
  }
  const bookmarks = document.getElementById("bookmarks");
  if (bookmarks) {
    [...bookmarks.getElementsByClassName('bookmarked__item')].forEach(elem => {
      if (elem) {
        const bookmarkText = elem.textContent.toLowerCase();
        if (bookmarkText.includes(searchTerm)) {
          elem.style.display = 'list-item';
        } else {
          elem.style.display = 'none';
        }
      }
    });
  }
}

function showBookmarkSearchIcon(searchIcon) {
  searchIcon.className = "";
  searchIcon.classList.add("fad");
  searchIcon.classList.add("fa-search");
}

function showCancelSearchIcon(searchIcon) {
  searchIcon.className = "";
  searchIcon.classList.add("fas");
  searchIcon.classList.add("fa-times");
}

function removeBookmarkSearch() {
  const search = document.getElementById("bookmark__search");
  const search_txt = document.getElementById("bookmark__search_txt");
  const search_icon = document.getElementById("bookmark__search_icon")
  if (search_icon) { search_icon.remove(); }
  if (search_txt) { search_txt.remove(); }
  if (search) { search.remove(); }
}

function editBookmark() {
  // hide menu if clicked while open
  if (bookmarkState === BKM_EDIT_MODE) {
    hideDropdowns();
    return;
  }
  enableEditAddMode();
}

function addBookmark() {
  enableEditAddMode();
  // create new unique menu ID
  let _newid;
  const _datalen = fileData.length;
  if (_datalen <= 1) {
    if (_datalen == 1) {
      _newid = 1;
    } else {
      _newid = 0;
    }
  } else {
    _newid = _datalen - 1;
  }
  // add temporary entry to menu array
  fileData.push("placeholder");
  // create placeholder menu item
  const navElement = document.getElementById('bookmarks');
  const _elm = document.createElement('a');
  _elm.id = "menu-" + _newid;
  _elm.innerText = "New Bookmark";
  _elm.classList.add('bookmarked__item');
  // store URL
  Object.defineProperty(_elm, "url", {
    enumerable: false,
    writable: true,
    value: "about:blank"
  });
  // define click action
  _elm.addEventListener("click", function() {
    clickBookmark(_newid);
  });
  // highlight selected item
  _elm.classList.add("dd-selected");
  // insert item below 
  navElement.insertBefore(_elm,navElement.firstChild);
  // add new window
  drawBookmarkPrompt(true, null, null, _elm);
}

function enableEditAddMode() {
  const elem = document.getElementById("bookmarks");
  if (elem) { // change color of menu
    elem.classList.add("bookmark-editmode");
  }
  // remove 
  removeBookmarkSearch();
  bookmarkState = BKM_EDIT_MODE;
  closeBookmarkPrompt();
}

function clickBookmark(id) {
  const elmid = "menu-" + id;
  const elem = document.getElementById(elmid);
  const url = Object.values(elem.url).join("");
  const name = elem.innerText;
  if (elem) { 
    if (bookmarkState === BKM_OPEN_MODE) {
      if (!(url == null || url == "" || url == "about:blank")) {
        // open URL in new tab
        window.open(url, "_blank");
      }
    }
    if (bookmarkState === BKM_EDIT_MODE) {
      // edit bookmark item
      closeBookmarkPrompt();
      elem.classList.add("dd-selected"); // highlight selected item
      drawBookmarkPrompt(false, url, name, elem);
    }
  }
}

async function drawBookmarkPrompt(add, url, name, elem){
  // create empty window
  let editFavPrompt = document.createElement("div"); 
  editFavPrompt.className = "editFav__win";
  editFavPrompt.id = "editFav__prompt";
  editFavPrompt.classList.add("prompt_win");
  // Link name edit box
  let editFavName = document.createElement("input"); 
  editFavName.type = "text";
  editFavName.value = name;
  editFavName.autocorrect = "off";
  editFavName.autocapitalize = "none"; 
  editFavName.id = "editFav__namebox";
  editFavName.classList.add("editFav__textbox");
  // URL edit box
  let editFavURL = document.createElement("input"); 
  editFavURL.type = "text";
  editFavURL.value = url;
  editFavURL.autocorrect = "off";
  editFavURL.autocapitalize = "none";
  editFavURL.id = "editFav__urlbox";
  editFavURL.classList.add("editFav__textbox");
  let bannerText;
  if (add === true) { // add new mode
    bannerText = "Add Bookmark";
    editFavName.placeholder = "Enter Name";
    editFavURL.placeholder = "Enter URL";
  } else { // edit mode 
    bannerText = "Edit Bookmark";
  }
  // window banner text
  let editFavText = document.createElement("div");
  editFavText.classList.add("editFav__toptext");
  editFavText.innerHTML = bannerText; 
  // cancel button
  let editFavCancelBtn = document.createElement("button");
  editFavCancelBtn.classList.add("editFav__button");
  editFavCancelBtn.classList.add("button");
  editFavCancelBtn.classList.add("fas");
  editFavCancelBtn.classList.add("fa-times");
  editFavCancelBtn.type = "button";
  // lookup favorites
  let editFavLookupBtn = document.createElement("button");
  editFavLookupBtn.classList.add("editFav__button");
  editFavLookupBtn.classList.add("button");
  editFavLookupBtn.classList.add("fad");
  editFavLookupBtn.classList.add("fa-search");
  editFavLookupBtn.type = "button";
  // save button
  let editFavSaveBtn = document.createElement("button");
  editFavSaveBtn.classList.add("editFav__button");
  editFavSaveBtn.classList.add("button");
  editFavSaveBtn.classList.add("fad");
  editFavSaveBtn.classList.add("fa-save");
  editFavSaveBtn.type = "button";
  // delete button
  let editFavDeleteBtn = document.createElement("button");
  editFavDeleteBtn.classList.add("editFav__button");
  editFavDeleteBtn.classList.add("button");
  editFavDeleteBtn.classList.add("fad");
  editFavDeleteBtn.classList.add("fa-trash-alt");
  editFavDeleteBtn.type = "button";
  // up button
  let editFavUpBtn = document.createElement("button");
  editFavUpBtn.classList.add("editFav__button");
  editFavUpBtn.classList.add("button");
  editFavUpBtn.classList.add("fad");
  editFavUpBtn.classList.add("fa-arrow-up");
  editFavUpBtn.type = "button"; 
  // down button
  let editFavDownBtn = document.createElement("button");
  editFavDownBtn.classList.add("editFav__button");
  editFavDownBtn.classList.add("button");
  editFavDownBtn.classList.add("fad");
  editFavDownBtn.classList.add("fa-arrow-down");
  editFavDownBtn.type = "button";
  // append elements to window
  editFavPrompt.appendChild(editFavText);      // window title text
  editFavPrompt.appendChild(editFavName);      // name text box
  editFavPrompt.appendChild(editFavURL);       // URL text box
  editFavPrompt.appendChild(editFavUpBtn);     // row #1
  editFavPrompt.appendChild(editFavLookupBtn); // row #1 
  editFavPrompt.appendChild(editFavDownBtn);   // row #2
  editFavPrompt.appendChild(editFavSaveBtn);   // row #2
  editFavPrompt.appendChild(editFavCancelBtn); // row #3
  editFavPrompt.appendChild(editFavDeleteBtn); // row #3
  // display window on page
  document.body.appendChild(editFavPrompt);
  // handle button actions
  new Promise(function() {
    editFavPrompt.addEventListener('click', function handleButtonClicks(btn_event) { 
    if (btn_event.target.tagName !== 'BUTTON') { return; }
      // move up button action
      if (btn_event.target === editFavUpBtn) {
        shiftMenuUp(elem);
      }
      // move down button action
      if (btn_event.target === editFavDownBtn) {
        shiftMenuDown(elem);
      }         
      // cancel button action
      if (btn_event.target === editFavCancelBtn) {
        editFavPrompt.removeEventListener('click', handleButtonClicks);
        hideDropdowns();
      }
      // lookup URL action
      if (btn_event.target === editFavLookupBtn) {
        lookupURL();
      }
      // save button action
      if (btn_event.target === editFavSaveBtn) {
        // do not allow empty URL or name
        var stopsave = false;
        if (editFavName.value == null || editFavName.value == "") {
          editFavName.placeholder = "Name cannot be empty";
          stopsave = true;
        }
        if (editFavURL.value == null || editFavURL.value == "") {
          editFavURL.placeholder = "URL cannot be empty";
          stopsave = true;
        }
        if (stopsave === true){
          return;
        }
        // update changed values
        if (elem) { 
          // save name to menu object
          let _boxname = editFavName.value.replaceAll("|", "-");
          // name length limiter
          let maxNameLength = 42;
          if (_boxname.length >= maxNameLength) {
            elem.innerText = _boxname.substring(0,maxNameLength) + "...";
          } else {
            elem.innerText = _boxname;
          }
          // save URL to menu object
          let _boxurl = addHTTPtoURL(editFavURL.value.replaceAll("|", "-"));
          elem['url'] = _boxurl;
          editFavURL.value = _boxurl;
          editFavText.innerHTML = "Changes Saved"; 
        }
        // save to file
        saveBookmarks();
        // close dropdown
        editFavPrompt.removeEventListener('click', handleButtonClicks);
        closeBookmarkPrompt();
      }
      // delete button action
      if (btn_event.target === editFavDeleteBtn) {
        elem.remove();
        // save to file
        saveBookmarks();
        // close dropdown
        editFavPrompt.removeEventListener('click', handleButtonClicks);
        closeBookmarkPrompt();
      }    
    });
  });   
}

// add HTTPs prefix if not defined
function addHTTPtoURL(linkin) {
  let linkout;
  if (linkin === "" || linkin === null) {
    linkout = "";
  } else {  
    if (linkin.toLowerCase().startsWith('http://', 0) || 
        linkin.toLowerCase().startsWith('https://', 0)) {
      linkout = linkin;
    } else {
      linkout = "https://" + linkin;
    }
  }
  return linkout;
}

// remove newlines
function cleanString(data) {
  if (data == null || data == undefined) {
    return null;
  } else {
    let out = data.replace(/(\r\n|\n|\r)/gm, '');
    return out;
  }
}

function base64URLSafeEncode(buffer) {
  return btoa(buffer)
    .replace(/\+/g, '-')  // Convert '+' to '-'
    .replace(/\//g, '_')  // Convert '/' to '_'
    .replace(/\=/g, '@'); // Convert '=' to '@'
}

async function lookupURL() {
  const urlBoxElem = document.getElementById("editFav__urlbox");
  const nameBoxElem = document.getElementById("editFav__namebox");
  if (urlBoxElem && nameBoxElem) {
    nameBoxElem.value = "Processing...";
    // read URL box
    let urlin = urlBoxElem.value;
    if (urlin == null || urlin == "") {
      // read from clipboard if URL empty
      urlin = await navigator.clipboard.readText();
    }
    // add HTTPs to URL
    let url = addHTTPtoURL(urlin);
    urlBoxElem.value = url;
    let encoded_url = base64URLSafeEncode(url);
    // search for URLs title
    sendCmd('main', 'sitelookup', encoded_url).then((data) => {
      if (data == null || data == "") {
        // URL lookup failed actions
        nameBoxElem.value = "Not Found";
        setTimeout(function() {
          if (urlBoxElem && nameBoxElem) {
            nameBoxElem.value = null;
            urlBoxElem.value = null;
          }
        }, 2000);
      } else { 
        // remove invalid characters, write to name box
        nameBoxElem.value = cleanString(data);
      }
    });
  }
}

function shiftMenuUp(elem) {
  if (elem) {
    if(elem.previousElementSibling)
      elem.parentNode.insertBefore(elem, elem.previousElementSibling);
  }
}

function shiftMenuDown(elem) {
  if (elem) {
    if(elem.nextElementSibling)
      elem.parentNode.insertBefore(elem.nextElementSibling, elem);
  }
}

function saveBookmarks() {
  let _file = "";
  const bookmarks = document.getElementById("bookmarks");
  if (!bookmarks) {
    return;
  }
  // loop through edited bookmarks
  [...bookmarks.getElementsByClassName('bookmarked__item')].forEach(elem => {
    if (elem) {
      const url = Object.values(elem.url).join("");
      const name = elem.innerText;
      // build output file
      _file += url + "|bkmrk|" + name + "\n";
    }
  });
  // transmit file
  savePOST('bookmarks', [_file]);
}

// close add / edit window
function closeBookmarkPrompt() {
  // re-enable click outside of window
  document.querySelectorAll("*:not(#editFav__prompt)").forEach(elem1 => {
    elem1.style.pointerEvents = "";
  }); 
  // un-highlight selected item
  for (var idx = 0; idx <= fileData.length; idx++) {
    const _menuid = "menu-" + idx;
    const elem2 = document.getElementById(_menuid);
    if (elem2) {
      if(elem2.classList.contains('dd-selected')) {
        elem2.classList.remove("dd-selected");
      }
    }
  }
  const favPrompt = document.getElementById("editFav__prompt");
  if (favPrompt) {
    favPrompt.remove();
  }  
}

function showHiFiMoreMenu() {
  const offcode = -1;
  const target = 'lrxmit';
  const menuname = 'hfmoremenu';
  let _elem = document.getElementById(menuname);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    hideDropdowns();
    _elem.style.display = 'block';
    // start spinner animation
    let btnText = document.getElementById(menuname + '-text');
    let btnSpinner = document.getElementById(menuname + '-spinner');
    btnText.style.visibility = 'hidden';
    btnSpinner.classList.add('btn-spinner');
    sendCmd('main', target, 'curhpfvollim').then((data) => { // GET request
      const resp = cleanString(data); // remove newlines
      let _indtype = 'noind'; // indicator type
      let _menudata = '';
      if (isNumeric(resp) === true || resp.includes('~')) {
        if (resp == offcode) {
          _menudata += menuname + '-offline|blkind|Offline\n';
        } else if (resp == 0) { 
          _menudata += menuname + '-muted|noind|Muted\n';
        } else {
          const field = resp.split("~"); // volLimitState ~ hpfState
          const limit_state = field[0]; 
          const hpf_state = field[1];
          // subwoofer volume toggle
          _menudata += 'subsbutton|hfsubs|Subs Gain\n';       
          // low-cut filter toggle
          if (hpf_state == '0') { _indtype = 'blkind'; }
          if (hpf_state == '1') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~togglehpf|' + _indtype + '|Low Cut\n';       
          // volume limiter toggle
          if (limit_state == '0') { _indtype = 'blkind'; }
          if (limit_state == '1') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~vlimit|' + _indtype + '|vLimiter\n';        
        }
      } else {
        _menudata += menuname + '-error|redind|Error\n';
      }
      // stop spinner animation
      btnText.style.visibility = 'visible';
      btnSpinner.classList.remove('btn-spinner');
      // draw menu items
      drawMenu(_menudata, menuname);
    });
  }
}

function showHiFiAmpInput() {
  const offcode = -1;
  const target = 'lrxmit';
  const menuname = 'hfinmenu';
  let _elem = document.getElementById(menuname);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    hideDropdowns();
    _elem.style.display = 'block';
    // start spinner animation
    let btnText = document.getElementById(menuname + '-text');
    let btnSpinner = document.getElementById(menuname + '-spinner');
    btnText.style.visibility = 'hidden';
    btnSpinner.classList.add('btn-spinner');
    sendCmd('main', target, 'curinput').then((data) => { // GET request
      const resp = cleanString(data); // remove newlines
      // draw menu items
      let _menudata = '';
      if (isNumeric(resp) === true) {
        if (resp == offcode) {
          _menudata += menuname + '-offline|blkind|Offline\n';
        } else { // online
          // auto input
          let _indtype = 'blkind'; // indicator type
          if (resp == '6') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~autodac|' + _indtype + '|Auto\n';
          // PC input
          _indtype = 'blkind';
          if (resp == '1') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~usb|' + _indtype + '|USB\n';
          // phono input
          _indtype = 'blkind';
          if (resp == '3') { _indtype = 'grnind'; }      
          _menudata += 'cmd~' + target + '~phono|' + _indtype + '|Phono\n';
          // optical input
          _indtype = 'blkind';
          if (resp == '5') { _indtype = 'grnind'; } 
          _menudata += 'cmd~' + target + '~opt|' + _indtype + '|Optical\n';
          // coaxial input
          _indtype = 'blkind';
          if (resp == '4') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~coax|' + _indtype + '|Coaxial\n';
          // aux analog input
          _indtype = 'blkind';
          if (resp == '2') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~aux|' + _indtype + '|Aux\n';
        }
      } else {
        _menudata += menuname + '-error|redind|Error\n';
      }
      // stop spinner animation
      btnText.style.visibility = 'visible';
      btnSpinner.classList.remove('btn-spinner');
      // draw menu items
      drawMenu(_menudata, menuname);
    });
  }
}

function showBedroomAmpInput() {
  const target = 'brpi';
  const menuname = 'brinpmenu';
  const offcode = 0;
  let _elem = document.getElementById(menuname);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    hideDropdowns();
    _elem.style.display = 'block';
    // start spinner animation
    let btnText = document.getElementById('ampinp-text');
    let btnSpinner = document.getElementById('ampinp-spinner');
    btnText.style.visibility = 'hidden';
    btnSpinner.classList.add('btn-spinner');
    sendCmd('main', target, 'inputstate').then((data) => { // GET request
      const resp = cleanString(data); // remove newlines
      let _menudata = '';
      if (isNumeric(resp) === true) {
        if (resp == offcode) {
          _menudata += menuname + '-offline|blkind|Offline\n';
        } else { // online
          // apple TV input 
          let _indtype = 'blkind';
          if (resp == '2') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~opt-a|' + _indtype + '|USB\n';
          // coaxial input
          _indtype = 'blkind';
          if (resp == '3') { _indtype = 'grnind'; }      
          _menudata += 'cmd~' + target + '~coaxial|' + _indtype + '|Coaxial\n';
          // aux optical input
          _indtype = 'blkind';
          if (resp == '1') { _indtype = 'grnind'; } 
          _menudata += 'cmd~' + target + '~opt-b|' + _indtype + '|Optical\n';
          // aux analog input
          _indtype = 'blkind';
          if (resp == '4') { _indtype = 'grnind'; }
          _menudata += 'cmd~' + target + '~aux|' + _indtype + '|Aux\n';
        }
      } else {
        _menudata += menuname + '-error|redind|Error\n';
      }
      // stop spinner animation
      btnText.style.visibility = 'visible';
      btnSpinner.classList.remove('btn-spinner');
      // draw menu items
      drawMenu(_menudata, menuname);
    });
  }
}

function showPowerMenu(target, menu) {
  let arg1;
  let arg2;
  if (target == "") {
    arg1 = menu;
    arg2 = "";
  } else {
    arg1 = target;
    arg2 = menu;
  }
  let _elem = document.getElementById(menu + '-menu');
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    hideDropdowns();
    // start spinner animation
    let btnText = document.getElementById(menu + '-text');
    let btnSpinner = document.getElementById(menu + '-spinner');
    btnText.style.visibility = 'hidden';
    btnSpinner.classList.add('btn-spinner');
    // show menu
    _elem.style.display = 'block';
    // custom actions
    let action = 'main';
    if (menu == 'fc_leds') {
      action = 'leds'
    }
    sendCmd(action, arg1, arg2).then((data) => { // GET request
      const resp = cleanString(data); // remove newlines
      const field = resp.split("~");
      const pri_cmd = field[0]; 
      const sub_cmd = field[1];
      let error = false;
      // draw menu items
      let _indtype; // indicator type
      let _title;   // menu button title
      let _menudata = "";
      // status display
      switch(pri_cmd) {
      case '0':
        _indtype = 'blkind';
        _title = "Offline";
        break;
      case '1':
        _indtype = 'grnind';
        _title = "Online";
        break;
      case 'pc_awake':
        _indtype = 'grnind';
        _title = "Online";
        break;
      default: // error response
        _indtype = 'redind';
        _title = "Error";
        error = true;
      }
      // custom menus
      switch(menu) {
      case 'fc_leds':
        // global fadecandy LEDs brightness
        _menudata += pri_cmd + '~leds~fc|slider|\n';
        break;
      default: 
        _menudata += target + '|' + _indtype + '|' + _title + '\n';
        if (error === false) {
          // power on/off buttons
          if (pri_cmd == '1') { // online
            _menudata += 'offcmd~' + target + '~' + menu + 'off|noind|Off\n';
          }
          if (pri_cmd == '0') { // offline
            _menudata += 'oncmd~' + target + '~' + menu + 'on|noind|On\n';
          }
          // custom buttons / items
          if (pri_cmd == 'pc_awake') {
            _menudata += 'sleepmode~' + target + '~' + menu + 'off|noind|Sleep\n';
          }
          // window lamp slider
          if (menu == 'wl_leds') {
            _menudata += sub_cmd + '~' + target + '~wl_leds_set|slider|\n';
          }
        }  
      }
      // stop spinner animation
      btnText.style.visibility = 'visible';
      btnSpinner.classList.remove('btn-spinner');
      // draw menu items
      drawMenu(_menudata, menu + '-menu');
    });
  }
}

function showStatusMenu() {
  const _menu = 'statsmenu';
  let statIcon = document.getElementById('navstats-icon');
  let statSpinner = document.getElementById('navstats-spinner');
  let _elem = document.getElementById(_menu);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
    // stop spinner animation
    statIcon.style.visibility = 'visible';
    statSpinner.classList.remove('right-nav-spinner');
  } else {
    hideDropdowns();
    // start spinner animation
    statIcon.style.visibility = 'hidden';
    statSpinner.classList.add('right-nav-spinner');
    // show menu
    _elem.style.display = 'block';
    sendCmd('main', 'status', '').then((data) => { // GET request
      // draw menu items
      drawMenu(data, _menu);
      // stop spinner animation
      statIcon.style.visibility = 'visible';
      statSpinner.classList.remove('right-nav-spinner');
    });
  }   
}

function afterPOSTActions(action) {
  let _send = false;
  if (action === 'pwd') {
    serverAction('attach_bkps');
    _send = true;
  }
  if (action === 'fusearch') {
    serverAction('files-mnt_arch_region');
    _send = true;
  }
  if (_send === true) {
    serverSend(0);
  }
}

// load server action 
function serverAction(cmd) {
  serverCmdData = cmd;
  // change color of send button
  sendBtnAlert("on");
}

function openLogWindow() {
  // open server log window
  closePopup();
  // show log form window
  document.getElementById("logForm").style.display = "block";
  // load log data
  loadLog('sysout');
}

// load entire text file
async function loadLog(file) {
  // build URL / append data
  const url = location.protocol + "//" + location.hostname + "/api/read?file=" + file;
  // read file action
  fetch(url, {
    method: 'GET'
  })
  .then((response) => {
    // check for HTTP errors
    if (!response.ok) {
      // trigger the catch block
      throw new Error;
    }
    // proceed to the next then if successful
    return response.json(); 
  })
  .then(data => {
    // display text on page
    const elmid = "logTextBox";
    document.getElementById(elmid).value = data;
    // scroll to bottom of page
    let txtArea = document.getElementById(elmid);
    txtArea.scrollTop = txtArea.scrollHeight;
  })
  .catch(error => {
    // This catches both the thrown HTTP error and any network errors
    showPopup("[loadLog] " + error.message);
  });
  serverCmdData = null;
}

// transmit a command
async function sendCmd(act, arg1, arg2) {
  // construct API URL
  const url = location.protocol + "//"+ location.hostname + "/api?var=" + arg2 + "&arg=" + arg1 + "&action=" + act;
  // send request
  try {
    const response = await fetch(url, {
      method: 'GET'
    });
    // return response
    let out = await response.json();
    return out;
  } catch (error) {
    showPopup("[sendCmd] " + error);
    return ERR_RESP;
  }
}

// save file API POST call
function savePOST(file, data) {
  const url = location.protocol + "//" + location.hostname + "/api/write?file=" + file;
  // submit request
  const xhr = new XMLHttpRequest();
  xhr.open("POST", url);
  xhr.setRequestHeader("Content-Type", "text/plain");
  xhr.onreadystatechange = function() {
    if (xhr.readyState === XMLHttpRequest.DONE) {
      if (xhr.status == 200) {
        // action on successful transmit
        afterPOSTActions(file);
      } else {
        showPopup("[savePOST] failed! " + file);
      }
    }
  }
  xhr.send(data);
}

//// Dynamic Menus ////

function showDynMenu(menu) {
  let _elem = document.getElementById(menu);
  if (_elem.style.display === 'block') {
    _elem.style.display = 'none';
  } else {
    hideDropdowns();
     _elem.style.display = 'block';
    // build URL / append data
    const url = location.protocol+ "//" +location.hostname + "/api/read?file=" + menu;
    loadMenu(url).then((data) => { // wait for response
      if (menu === 'bookmarks') {
        showBookmarkSearch();
      }
      drawMenu(data, menu);
    });  
  }
}

async function loadMenu(url) {
  try {
    const response = await fetch(url, {
      method: "GET"
    });
    // return response
    return out = await response.json();
  } catch (error) {
    showPopup("[loadMenu] " + error);
  }
}

// draws each menu item
function drawMenu(data, menu) {
  // remove any current dynamic menus
  removeDynMenus();
  if (!(data === null || data === "")) {
    // erase global data
    while (fileData.length) { 
      fileData.pop(); 
    }
    let splitdat = data.split("\n");
    let rows = splitdat.length;
    for (let row = 0; row < rows; row++) {
      let line = splitdat[row];
      if (line != "") {
        fileData.push(line); // write to array
        const navElement = document.getElementById(menu);
        const linearr = line.split("|");
        // 0=Host, 1=Type, 2=Name
        const col0 = linearr[0];
        const col1 = linearr[1];
        const col2 = linearr[2].trim();
        // draw menu item
        navElement.appendChild(createListItem(col0, col1, col2, row));
        dynMenuActive = 1;
      }
    }
    // store menu name at end of array
    fileData.push(menu);
  } else {
    showPopup("[drawMenu] no data!");
  }
}

function createListItem(col0_in, col1, col2, menuid) {
  //console.log(col0_in, col1, col2);
  let _elm = document.createElement('a');
  let _dot = document.createElement('span');
  let field = col0_in.split("~");
  let col0 = field[0];
  let host = field[1];
  let cmd = field[2];
  // assign menu ID
  _elm.id = "menu-" + menuid;
  // set menu text
  _elm.innerText = col2;
  // configure menu item
  switch (col1) {
    case 'chkon': 
      // check-box checked
      drawCheckBoxItem(_elm, col0, col2, true);
      break;
    case 'chkoff': 
      // check-box unchecked
      drawCheckBoxItem(_elm, col0, col2, false);
      break;
    case 'ledmenu': 
      // LEDs menu
      const effect_cmd = col0;
      _elm.addEventListener("click", function() {
        if (effect_cmd == 'textscroll') {
          openTextScroll();
        } else {
          sendCmd('leds',effect_cmd,'');
        }
      });
      break;
    case 'relaxmenu': 
      // relax menu
      _elm.addEventListener("click", function() {
        relaxSend(col0);
      });
      break;
    case 'link': 
      // URL menu
      _elm.href = col0;
      break;
    case 'thm': 
      // theme menu 
      _elm.classList.add('theme-colorbox');
      _elm.style.setProperty('background-color', col0);
      _elm.addEventListener("click", function() {
        setTheme(col0);
      });
      break;
    case 'slider':
      // horizontal slider menu
      const maxRange = 255;
      const slider = document.createElement('input');
      slider.classList.add("slider-wide");
      slider.type = 'range';
      slider.min = 0;
      slider.max = maxRange;
      slider.value = col0;
      slider.step = 1;
      slider.addEventListener('input', (event) => {
        throttledSliderSend(host, cmd, event.target.value);
      });
      _elm.appendChild(slider);
      _elm.classList.add('slider-wide_container');
      break;
    case 'bkmrk': 
      // bookmarks menu 
      _elm.classList.add('bookmarked__item');
      Object.defineProperty(_elm, "url", {
        enumerable: false,
        writable: true,
        value: col0
      });
      _elm.addEventListener("click", function() {
        clickBookmark(menuid);
      });
      break;
    case 'hfsubs':
      // subwoofer volume toggle
      if (ctlCommand == 'subs' ) {
        _elm.classList.add('submode_btn');
      }
      _elm.addEventListener("click", function() {
        subModeToggle();
      });
      break;
    case 'noind': 
      // no indicator type status menu
      drawStatusItem(_elm, col0, host, cmd);
      break;
    case 'blkind': 
      // black indicator type status menu
      _dot.classList.add('ind_dot');
      _dot.id = "ind-" + col2;
      drawStatusItem(_elm, col0, host, cmd);
      _elm.appendChild(_dot);
      break;
    case 'grnind': 
      // green indicator type status menu
      _dot.classList.add('ind_dot');
      _dot.classList.add('ind_dot_green');
      _dot.id = "ind-" + col2;
      drawStatusItem(_elm, col0, host, cmd);
      _elm.appendChild(_dot);
      break;
    case 'redind': 
      // red indicator type status menu
      _dot.classList.add('ind_dot');
      _dot.classList.add('ind_dot_red');
      _dot.id = "ind-" + col2;
      drawStatusItem(_elm, col0, host, cmd);
      _elm.appendChild(_dot);
      break;
    case 'ylwind': 
      // yellow indicator type status menu
      _dot.classList.add('ind_dot');
      _dot.classList.add('ind_dot_yellow');
      _dot.id = "ind-" + col2;
      drawStatusItem(_elm, col0, host, cmd);
      _elm.appendChild(_dot);
      break;
  }
  return _elm;
}

const throttledSliderSend = throttle((host, cmd, val) => {
  sendCmd(host, cmd, val);
}, 50); // in (ms)

function sendPowerCommand(host, cmd) {
  sendCmd('main', host, cmd);
}

function drawStatusItem(elm, col0, host, cmd) {
  const _icon = document.createElement('span');
  if (col0 == 'oncmd') { // power-on remote API call on click
    _icon.classList.add('fa');
    _icon.classList.add('fa-toggle-on');
    _icon.classList.add('left-jfy'); // left-justify icon
    elm.appendChild(_icon);
    elm.addEventListener("click", function() {
      sendPowerCommand(host, cmd);
    });
  }
  if (col0 == 'offcmd') { // power-off remote API call on click
    _icon.classList.add('fa');
    _icon.classList.add('fa-toggle-off');
    _icon.classList.add('left-jfy'); // left-justify icon
    elm.appendChild(_icon);
    elm.addEventListener("click", function() {
      sendPowerCommand(host, cmd);
    });
  }
  if (col0 == 'sleepmode') { // sleep PC type menu
    _icon.classList.add('fa');
    _icon.classList.add('fa-moon');
    _icon.classList.add('left-jfy'); // left-justify icon
    elm.appendChild(_icon);
    elm.addEventListener("click", function() {
      sendPowerCommand(host, cmd);
    });
  }
  if (col0 == 'cmd') { // generic remote API call on click
    elm.addEventListener("click", function() {
      sendCmd('main', host, cmd);
    });
  }
}

function drawCheckBoxItem(elm, url, boxid, state) {
  const _cbox = document.createElement('input');
  _cbox.type = "checkbox";
  _cbox.className = "chkbox";
  _cbox.id = "chkbox-" + boxid;
  // read checkbox state from file
  _cbox.checked = state;
  elm.appendChild(_cbox);
  // URL on click
  elm.href = url;
  // a checkbox was clicked
  elm.addEventListener('click', function(event) {
    if (event.target.classList.contains('chkbox')) {
      boxChanged();
    }
  });
}

function boxChanged() {
  // loop through checkbox's state
  for (var i = 0; i < (fileData.length - 1); i++) {
    let line = fileData[i];
    // split up into array (host,state,name)
    const linearr = line.split("|");
    // only write box state on 0/1 state items
    if (linearr[1] == 'chkon' || linearr[1] == 'chkoff') {
      // read elements checkbox then write state
      const box = "chkbox-" + linearr[2];
      var boxelm = document.getElementById(box);
      if (boxelm.checked === true) {
        linearr[1] = 'chkon';
      } else {
        linearr[1] = 'chkoff';
      }
      // build new data
      let _outstr = linearr.join('|');
      // replace existing data
      fileData[i] = _outstr;
    }
  }
  // write checkbox changes to file (III)
  let menuid = fileData[fileData.length - 1]; // extract menu ID
  // remove last element of array
  fileData.pop();
  // update file
  let fileout = "";
  for (var row = 0; row < fileData.length; row++) {
    fileout += fileData[row] + '\n';
  }
  savePOST(menuid, fileout);
  // add menu ID back to array
  fileData.push(menuid);
}

function removeDynMenus() {
  // only if menu is on-screen 
  if (dynMenuActive == 1) {
    // remove dynamic menu elements (II)
    for (var idx = 0; idx <= fileData.length; idx++) {
      const _menuid = "menu-" + idx;
      const menuRemove = document.getElementById(_menuid);
      if (menuRemove != null) {
        menuRemove.remove();
      }
    }
    dynMenuActive = 0;
  }
}

//// End Dynamic Menus ////

// show camera form window
function openCamWindow() {
  closePopup();
  document.getElementById("cam-popup").style.display = "block";
  document.getElementById("cam-display").src = "/cam1";
}

function goToContextRoot(_path) {
  closePopup();
  window.location = location.protocol + "//" + location.hostname + "/" + _path;
}

// close all popup windows
function closePopup() {
  document.getElementById("logForm").style.display = "none";
  document.getElementById("cam-popup").style.display = "none";
  document.getElementById("cam-display").src = "";
  clearPendingCmd();
  closeServerOptions();
}

function animatedBackground() {
  const BG_COLOR = '#000';
  
  // Background Stars
  const BG_STAR_DENSITY = 2000; 
  const BG_STAR_MAX_RADIUS = 1.3;
  const BG_STAR_MIN_ALPHA = 0.1;
  const BG_STAR_MAX_ALPHA = 0.8;
  const BG_STAR_FLICKER_BASE_SPEED = 0.005;
  const BG_STAR_FLICKER_VARIANCE = 0.001;
  
  // Shooting Stars
  const MAX_SH_STARS = 6;
  const SH_STAR_MIN_SPEED = 5;
  const SH_STAR_SPEED_VARIANCE = 10;
  const SH_STAR_MIN_LENGTH = 50;
  const SH_STAR_LENGTH_VARIANCE = 150;
  const SH_STAR_MIN_THICKNESS = 0.3;
  const SH_STAR_THICKNESS_VARIANCE = 1.2;
  const SH_STAR_BASE_ANGLE = Math.PI / 4; 
  const SH_STAR_ANGLE_VARIANCE = 0.05;
  const SH_STAR_START_Y_OFFSET = -150;

  const canvas = document.getElementById('star-canvas');
  const ctx = canvas.getContext('2d');

  let backgroundStars = [];
  let shootingStars = [];

  function resizeCanvas() {
    // OPTIMIZATION: Cap the Device Pixel Ratio to 1.5 on low-end devices 
    // to prevent mobile phones from rendering at 3x or 4x resolution unnecessarily.
    const dpr = Math.min(window.devicePixelRatio || 1, 1.5);
    
    canvas.width = window.innerWidth * dpr;
    canvas.height = window.innerHeight * dpr;
    canvas.style.width = `${window.innerWidth}px`;
    canvas.style.height = `${window.innerHeight}px`;
    ctx.scale(dpr, dpr);
    initBackgroundStars();
  }

  class BackgroundStar {
    constructor() {
      this.x = Math.random() * window.innerWidth;
      this.y = Math.random() * window.innerHeight;
      // OPTIMIZATION: Calculate size once. (Radius * 2)
      this.size = (Math.random() * BG_STAR_MAX_RADIUS) * 2;
      this.alpha = Math.random();
      this.alphaChange = (Math.random() * BG_STAR_FLICKER_VARIANCE) + BG_STAR_FLICKER_BASE_SPEED;
    }
    update() {
      this.alpha += this.alphaChange;
      if (this.alpha <= BG_STAR_MIN_ALPHA || this.alpha >= BG_STAR_MAX_ALPHA) {
        this.alphaChange = -this.alphaChange;
      }  
      
      ctx.globalAlpha = this.alpha;
      // OPTIMIZATION: Replaced arc() with fillRect(). Drawing tiny squares is 
      // drastically faster than drawing tiny circles, and visually indistinguishable.
      ctx.fillRect(this.x, this.y, this.size, this.size);
    }
  }

  class ShootingStar {
    constructor() {
      this.reset();
      this.delay = Math.random() * 300; 
    }
    reset() {
      this.x = Math.random() * window.innerWidth * 0.5 - (window.innerWidth * 0.2); 
      this.y = Math.random() * SH_STAR_START_Y_OFFSET; 
      
      const speed = (Math.random() * SH_STAR_SPEED_VARIANCE) + SH_STAR_MIN_SPEED; 
      this.length = (Math.random() * SH_STAR_LENGTH_VARIANCE) + SH_STAR_MIN_LENGTH;
      this.thickness = (Math.random() * SH_STAR_THICKNESS_VARIANCE) + SH_STAR_MIN_THICKNESS;
      this.angle = SH_STAR_BASE_ANGLE + (Math.random() * (SH_STAR_ANGLE_VARIANCE * 2) - SH_STAR_ANGLE_VARIANCE); 
      
      // OPTIMIZATION: Pre-calculate velocity vectors here so we don't 
      // compute Math.sin() and Math.cos() 60 times a second per star.
      this.vx = speed * Math.cos(this.angle);
      this.vy = speed * Math.sin(this.angle);
      
      // OPTIMIZATION: Cache the gradient. Instead of creating a new linear 
      // gradient every frame, we create it once per star lifespan.
      this.gradient = ctx.createLinearGradient(0, 0, -this.length, 0);
      this.gradient.addColorStop(0, '#fff');
      this.gradient.addColorStop(1, 'rgba(255, 255, 255, 0)');
      
      this.delay = Math.random() * 200 + 50; 
    }
    update() {
      if (this.delay > 0) {
        this.delay--;
        return; 
      }
      
      // Use pre-calculated velocities
      this.x += this.vx;
      this.y += this.vy;
      
      if (this.x > window.innerWidth + this.length || 
          this.y > window.innerHeight + this.length) {
        this.reset();
      } else {
        ctx.save();
        ctx.translate(this.x, this.y);
        ctx.rotate(this.angle);
        
        ctx.strokeStyle = this.gradient;
        ctx.lineWidth = this.thickness;
        ctx.lineCap = 'round';
        ctx.beginPath();
        ctx.moveTo(0, 0);
        ctx.lineTo(-this.length, 0);
        ctx.stroke();
        
        ctx.restore();
      }
    }
  }

  function initBackgroundStars() {
    backgroundStars = [];
    shootingStars = [];
    // Slight optimization using bitwise NOT for faster flooring
    const numStars = ~~((window.innerWidth * window.innerHeight) / BG_STAR_DENSITY);
    
    for (let i = 0; i < numStars; i++) {
      backgroundStars.push(new BackgroundStar());
    }
    for (let i = 0; i < MAX_SH_STARS; i++) {
      shootingStars.push(new ShootingStar());
    }
  }

  function animateCanvas() {
    ctx.globalAlpha = 1; 
    ctx.fillStyle = BG_COLOR;
    ctx.fillRect(0, 0, window.innerWidth, window.innerHeight);
    
    // OPTIMIZATION: Set fillStyle ONCE for all background stars, rather 
    // than setting it individually inside the class method on every loop.
    ctx.fillStyle = '#fff';
    
    // OPTIMIZATION: Standard for-loops are traditionally faster than .forEach
    for (let i = 0; i < backgroundStars.length; i++) {
      backgroundStars[i].update();
    }
    
    // Reset alpha for shooting stars
    ctx.globalAlpha = 1; 
    for (let i = 0; i < shootingStars.length; i++) {
      shootingStars[i].update();
    }
    
    requestAnimationFrame(animateCanvas);
  }

  window.addEventListener('resize', resizeCanvas);
  resizeCanvas();
  animateCanvas();
}
