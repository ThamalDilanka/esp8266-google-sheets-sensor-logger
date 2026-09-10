/*
 * Google Apps Script receiver for the indoor/outdoor climate logger.
 *
 * Set SHEET_ID and DEVICE_TOKEN in Project Settings > Script Properties,
 * run setup() once, then deploy this project as a web app.
 */

const SHEET_NAME = 'Measurements';
const TIME_ZONE = 'Asia/Colombo';
const LOCK_TIMEOUT_MS = 30000;
const HEADERS = [
  'Timestamp',
  'Indoor Temp °C',
  'Indoor RH %',
  'Outdoor Temp °C',
  'Outdoor RH %',
];
const READING_FIELDS = ['indoor_t', 'indoor_rh', 'outdoor_t', 'outdoor_rh'];

function doGet() {
  return jsonResponse({ ok: true, service: 'climate-logger' });
}

function doPost(e) {
  const receivedAt = new Date();
  let configuration;
  try {
    configuration = readConfiguration();
  } catch (ignored) {
    return errorResponse('configuration_error');
  }

  const tokenValues = e && e.parameters && e.parameters.token;
  if (!e || !e.parameter || e.parameter.token !== configuration.deviceToken ||
      (Array.isArray(tokenValues) && tokenValues.length !== 1)) {
    return errorResponse('unauthorized');
  }

  const parsed = parseReadings(e);
  if (parsed.error) {
    return errorResponse(parsed.error);
  }

  let lock;
  try {
    lock = LockService.getScriptLock();
    if (!lock.tryLock(LOCK_TIMEOUT_MS)) {
      return errorResponse('lock_unavailable');
    }
  } catch (ignored) {
    return errorResponse('lock_unavailable');
  }

  try {
    const spreadsheet = SpreadsheetApp.openById(configuration.sheetId);
    const sheet = prepareSheet(spreadsheet);
    if (!sheet) {
      return errorResponse('header_mismatch');
    }

    sheet.appendRow([
      receivedAt,
      parsed.values.indoor_t,
      parsed.values.indoor_rh,
      parsed.values.outdoor_t,
      parsed.values.outdoor_rh,
    ]);
    const row = sheet.getLastRow();
    // Appending can grow the grid beyond the rows formatted by prepareSheet.
    formatMeasurementRows(sheet, row, 1);
    SpreadsheetApp.flush();

    return jsonResponse({
      ok: true,
      row,
      timestamp: receivedAt.toISOString(),
    });
  } catch (ignored) {
    return errorResponse('storage_error');
  } finally {
    releaseLock(lock);
  }
}

function setup() {
  let configuration;
  try {
    configuration = readConfiguration();
  } catch (ignored) {
    throw new Error('Setup failed: check SHEET_ID and DEVICE_TOKEN Script Properties.');
  }

  let lock;
  try {
    lock = LockService.getScriptLock();
    if (!lock.tryLock(LOCK_TIMEOUT_MS)) {
      throw new Error('Lock unavailable');
    }
  } catch (ignored) {
    throw new Error('Setup failed: could not acquire the script lock. Try again.');
  }

  let failure = null;
  try {
    const spreadsheet = SpreadsheetApp.openById(configuration.sheetId);
    if (!prepareSheet(spreadsheet)) {
      failure = 'header_mismatch';
    } else {
      SpreadsheetApp.flush();
    }
  } catch (ignored) {
    failure = 'storage_error';
  } finally {
    releaseLock(lock);
  }

  if (failure === 'header_mismatch') {
    throw new Error('Setup failed: Measurements has incompatible headers.');
  }
  if (failure === 'storage_error') {
    throw new Error('Setup failed: could not initialize Measurements.');
  }

  Logger.log('Climate logger setup complete: Measurements is ready.');
}

function readConfiguration() {
  const properties = PropertiesService.getScriptProperties();
  const sheetId = properties.getProperty('SHEET_ID');
  const deviceToken = properties.getProperty('DEVICE_TOKEN');

  if (typeof sheetId !== 'string' || sheetId.trim() === '' ||
      typeof deviceToken !== 'string' || !/^[0-9a-fA-F]{32}$/.test(deviceToken)) {
    throw new Error('Invalid script properties');
  }

  return { sheetId: sheetId.trim(), deviceToken };
}

function parseReadings(e) {
  if (e.parameters) {
    for (let i = 0; i < READING_FIELDS.length; i += 1) {
      const field = READING_FIELDS[i];
      if (Array.isArray(e.parameters[field]) && e.parameters[field].length !== 1) {
        return { error: 'duplicate_field' };
      }
    }
  }

  const values = {};
  const decimalPattern = /^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$/;
  for (let i = 0; i < READING_FIELDS.length; i += 1) {
    const field = READING_FIELDS[i];
    const raw = e.parameter[field];
    if (typeof raw !== 'string' || raw === '' || raw.trim() !== raw ||
        !decimalPattern.test(raw)) {
      return { error: 'invalid_number' };
    }

    const value = Number(raw);
    if (!Number.isFinite(value)) {
      return { error: 'invalid_number' };
    }
    values[field] = value;
  }

  if (values.indoor_t < -40 || values.indoor_t > 80 ||
      values.outdoor_t < -40 || values.outdoor_t > 80 ||
      values.indoor_rh < 0 || values.indoor_rh > 99.9 ||
      values.outdoor_rh < 0 || values.outdoor_rh > 99.9) {
    return { error: 'out_of_range' };
  }

  return { values };
}

function prepareSheet(spreadsheet) {
  spreadsheet.setSpreadsheetTimeZone(TIME_ZONE);
  let sheet = spreadsheet.getSheetByName(SHEET_NAME);
  if (!sheet) {
    sheet = spreadsheet.insertSheet(SHEET_NAME);
  }

  if (sheet.getLastRow() === 0) {
    sheet.getRange(1, 1, 1, HEADERS.length).setValues([HEADERS]);
  } else {
    const actualHeaders = sheet.getRange(1, 1, 1, HEADERS.length).getValues()[0];
    for (let i = 0; i < HEADERS.length; i += 1) {
      if (actualHeaders[i] !== HEADERS[i]) {
        return null;
      }
    }
  }

  sheet.setFrozenRows(1);
  formatMeasurementRows(sheet, 2, sheet.getMaxRows() - 1);
  return sheet;
}

function formatMeasurementRows(sheet, firstRow, rowCount) {
  if (rowCount <= 0) return;
  sheet.getRange(firstRow, 1, rowCount, 1).setNumberFormat('yyyy-mm-dd hh:mm:ss');
  sheet.getRange(firstRow, 2, rowCount, 4).setNumberFormat('0.0');
}

function releaseLock(lock) {
  try {
    lock.releaseLock();
  } catch (ignored) {
    // Apps Script releases locks at the end of execution; keep the response stable.
  }
}

function errorResponse(code) {
  return jsonResponse({ ok: false, error: code });
}

function jsonResponse(value) {
  return ContentService
    .createTextOutput(JSON.stringify(value))
    .setMimeType(ContentService.MimeType.JSON);
}
