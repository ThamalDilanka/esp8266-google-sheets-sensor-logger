const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const SCRIPT_PATH = path.resolve(__dirname, '../../apps-script/Code.gs');
const HEADERS = [
  'Timestamp',
  'Indoor Temp °C',
  'Indoor RH %',
  'Outdoor Temp °C',
  'Outdoor RH %',
];
const VALID_TOKEN = '0123456789abcdef0123456789abcdef';

function createHarness(options = {}) {
  const state = {
    propertiesRead: 0,
    lockRequests: 0,
    lockAttempts: 0,
    lockHeld: false,
    lockReleases: 0,
    spreadsheetOpens: 0,
    flushes: 0,
    timezone: options.timezone || 'Etc/GMT',
    logs: [],
    numberFormats: new Map(),
    maxRows: options.maxRows ?? 1000,
    frozenRows: options.frozenRows || 0,
    rows: options.rows ? options.rows.map((row) => row.slice()) : [],
    sheetExists: options.sheetExists !== false,
  };

  const sheet = {
    getMaxRows() {
      return state.maxRows;
    },
    getLastRow() {
      assert.equal(state.lockHeld, true, 'sheet access must occur while holding the script lock');
      return state.rows.length;
    },
    getRange(row, column, rowCount, columnCount) {
      assert.equal(state.lockHeld, true, 'sheet access must occur while holding the script lock');
      assert.ok(row >= 1 && rowCount >= 1 && row + rowCount - 1 <= state.maxRows,
        'formatting and data access must stay within the allocated sheet rows');
      return {
        getValues() {
          return Array.from({ length: rowCount }, (_, rowOffset) =>
            Array.from({ length: columnCount }, (_, columnOffset) =>
              state.rows[row - 1 + rowOffset]?.[column - 1 + columnOffset] ?? '',
            ),
          );
        },
        setValues(values) {
          for (let r = 0; r < values.length; r += 1) {
            const targetRow = row - 1 + r;
            if (!state.rows[targetRow]) state.rows[targetRow] = [];
            for (let c = 0; c < values[r].length; c += 1) {
              state.rows[targetRow][column - 1 + c] = values[r][c];
            }
          }
          return this;
        },
        setNumberFormat(format) {
          for (let r = row; r < row + rowCount; r += 1) {
            for (let c = column; c < column + columnCount; c += 1) {
              state.numberFormats.set(`${r}:${c}`, format);
            }
          }
          return this;
        },
      };
    },
    appendRow(row) {
      assert.equal(state.lockHeld, true, 'append must occur while holding the script lock');
      if (options.appendError) throw new Error('private append details');
      state.rows.push(row.slice());
      state.maxRows = Math.max(state.maxRows, state.rows.length);
      return sheet;
    },
    setFrozenRows(count) {
      assert.equal(state.lockHeld, true, 'sheet configuration must hold the script lock');
      state.frozenRows = count;
      return sheet;
    },
  };

  const spreadsheet = {
    getSheetByName(name) {
      assert.equal(state.lockHeld, true, 'sheet initialization must hold the script lock');
      assert.equal(name, 'Measurements');
      return state.sheetExists ? sheet : null;
    },
    insertSheet(name) {
      assert.equal(state.lockHeld, true, 'sheet creation must hold the script lock');
      assert.equal(name, 'Measurements');
      state.sheetExists = true;
      return sheet;
    },
    setSpreadsheetTimeZone(timezone) {
      assert.equal(state.lockHeld, true, 'spreadsheet configuration must hold the script lock');
      state.timezone = timezone;
      return spreadsheet;
    },
  };

  const properties = {
    SHEET_ID: 'sheet-id-123',
    DEVICE_TOKEN: VALID_TOKEN,
    ...options.properties,
  };
  const lock = {
    tryLock(timeout) {
      state.lockAttempts += 1;
      state.lockTimeout = timeout;
      if (options.lockError) throw new Error('private lock details');
      state.lockHeld = options.lockAcquired !== false;
      return state.lockHeld;
    },
    releaseLock() {
      state.lockReleases += 1;
      state.lockHeld = false;
    },
  };
  const sandbox = {
    ContentService: {
      MimeType: { JSON: 'application/json' },
      createTextOutput(text) {
        return {
          text,
          mimeType: null,
          setMimeType(mimeType) {
            this.mimeType = mimeType;
            return this;
          },
        };
      },
    },
    Logger: {
      log(message) {
        state.logs.push(message);
      },
    },
    PropertiesService: {
      getScriptProperties() {
        state.propertiesRead += 1;
        if (options.propertiesError) throw new Error('private property details');
        return {
          getProperty(name) {
            return Object.prototype.hasOwnProperty.call(properties, name)
              ? properties[name]
              : null;
          },
        };
      },
    },
    LockService: {
      getScriptLock() {
        state.lockRequests += 1;
        return lock;
      },
    },
    SpreadsheetApp: {
      openById(id) {
        assert.equal(state.lockHeld, true, 'storage access must occur while holding the script lock');
        state.spreadsheetOpens += 1;
        state.openStartedAt = Date.now();
        if (options.openDelayMs) {
          const finishAt = state.openStartedAt + options.openDelayMs;
          while (Date.now() < finishAt) {
            // Simulate latency at the unavailable Google Sheets boundary.
          }
        }
        if (options.openError) throw new Error('private storage details');
        assert.equal(id, properties.SHEET_ID);
        return spreadsheet;
      },
      flush() {
        assert.equal(state.lockHeld, true, 'flush must occur while holding the script lock');
        state.flushes += 1;
        if (options.flushError) throw new Error('private flush details');
      },
    },
  };

  const source = fs.readFileSync(SCRIPT_PATH, 'utf8');
  vm.runInNewContext(source, sandbox, { filename: SCRIPT_PATH });
  return { api: sandbox, state };
}

function event(overrides = {}, duplicateOverrides = {}) {
  const parameter = {
    token: VALID_TOKEN,
    indoor_t: '21.5',
    indoor_rh: '55.1',
    outdoor_t: '-2.5',
    outdoor_rh: '99.9',
    ...overrides,
  };
  const parameters = Object.fromEntries(
    Object.entries(parameter).map(([name, value]) => [name, [value]]),
  );
  Object.assign(parameters, duplicateOverrides);
  return { parameter, parameters };
}

function json(output) {
  assert.equal(output.mimeType, 'application/json');
  return JSON.parse(output.text);
}

function assertNoWrite(state) {
  assert.equal(state.lockRequests, 0);
  assert.equal(state.spreadsheetOpens, 0);
  assert.equal(state.flushes, 0);
}

test('doGet returns public health metadata without reading configuration or sheet data', () => {
  const { api, state } = createHarness();

  assert.deepEqual(json(api.doGet()), { ok: true, service: 'climate-logger' });
  assert.equal(state.propertiesRead, 0);
  assertNoWrite(state);
});

test('doPost rejects missing and incorrect device tokens before locking or storage access', async (t) => {
  for (const [name, request] of [
    ['missing event', undefined],
    ['missing parameters', {}],
    ['missing token', event({ token: undefined })],
    ['blank token', event({ token: '' })],
    ['wrong token', event({ token: 'ffffffffffffffffffffffffffffffff' })],
  ]) {
    await t.test(name, () => {
      const { api, state } = createHarness();
      assert.deepEqual(json(api.doPost(request)), { ok: false, error: 'unauthorized' });
      assertNoWrite(state);
    });
  }
});

test('doPost rejects duplicate token parameters as unauthorized before any write', () => {
  const { api, state } = createHarness();
  const request = event({}, { token: [VALID_TOKEN, 'invalid'] });

  assert.deepEqual(json(api.doPost(request)), { ok: false, error: 'unauthorized' });
  assertNoWrite(state);
});

test('doPost rejects a configured token unless it is exactly 32 hexadecimal characters', async (t) => {
  for (const configuredToken of [
    '',
    '0123456789abcdef0123456789abcde',
    '0123456789abcdef0123456789abcdef0',
    '0123456789abcdef0123456789abcdeg',
  ]) {
    await t.test(JSON.stringify(configuredToken), () => {
      const { api, state } = createHarness({ properties: { DEVICE_TOKEN: configuredToken } });
      assert.deepEqual(json(api.doPost(event())), { ok: false, error: 'configuration_error' });
      assertNoWrite(state);
    });
  }
});

test('doPost rejects absent, blank, malformed, and nonfinite readings before any write', async (t) => {
  for (const [name, field, value] of [
    ['absent', 'indoor_t', undefined],
    ['empty', 'indoor_rh', ''],
    ['whitespace', 'outdoor_t', '  '],
    ['malformed', 'outdoor_rh', '55x'],
    ['infinity', 'indoor_t', 'Infinity'],
    ['NaN', 'indoor_rh', 'NaN'],
  ]) {
    await t.test(name, () => {
      const { api, state } = createHarness();
      assert.deepEqual(json(api.doPost(event({ [field]: value }))), {
        ok: false,
        error: 'invalid_number',
      });
      assertNoWrite(state);
    });
  }
});

test('doPost rejects duplicate numeric form fields before any write', async (t) => {
  for (const field of ['indoor_t', 'indoor_rh', 'outdoor_t', 'outdoor_rh']) {
    await t.test(field, () => {
      const { api, state } = createHarness();
      assert.deepEqual(
        json(api.doPost(event({}, { [field]: ['12.0', '13.0'] }))),
        { ok: false, error: 'duplicate_field' },
      );
      assertNoWrite(state);
    });
  }
});

test('doPost rejects temperatures and humidity outside sensor limits', async (t) => {
  for (const [field, value] of [
    ['indoor_t', '-40.1'],
    ['indoor_t', '80.1'],
    ['outdoor_t', '-40.1'],
    ['outdoor_t', '80.1'],
    ['indoor_rh', '-0.1'],
    ['indoor_rh', '100'],
    ['outdoor_rh', '-0.1'],
    ['outdoor_rh', '100'],
  ]) {
    await t.test(`${field}=${value}`, () => {
      const { api, state } = createHarness();
      assert.deepEqual(json(api.doPost(event({ [field]: value }))), {
        ok: false,
        error: 'out_of_range',
      });
      assertNoWrite(state);
    });
  }
});

test('doPost accepts zero, negative temperatures, and all inclusive boundaries', async (t) => {
  for (const values of [
    { indoor_t: '0', indoor_rh: '0', outdoor_t: '-0.1', outdoor_rh: '0' },
    { indoor_t: '-40', indoor_rh: '99.9', outdoor_t: '80', outdoor_rh: '99.9' },
    { indoor_t: '80', indoor_rh: '0', outdoor_t: '-40', outdoor_rh: '0' },
  ]) {
    await t.test(JSON.stringify(values), () => {
      const { api, state } = createHarness();
      assert.equal(json(api.doPost(event(values))).ok, true);
      assert.equal(state.rows.length, 2);
    });
  }
});

test('doPost appends one timestamped numeric row in the documented column order', () => {
  const existing = [HEADERS, ['old date', 20, 50, 25, 60]];
  const { api, state } = createHarness({ rows: existing });
  const before = Date.now();

  const response = json(api.doPost(event()));
  const after = Date.now();
  assert.equal(response.ok, true);
  assert.equal(response.row, 3);
  assert.equal(Number.isInteger(response.row), true);
  assert.equal(response.row >= 2, true);
  assert.equal(typeof response.timestamp, 'string');
  assert.equal(Date.parse(response.timestamp) >= before, true);
  assert.equal(Date.parse(response.timestamp) <= after, true);
  assert.equal(state.rows.length, 3);
  assert.equal(state.rows[2][0].toISOString(), response.timestamp);
  assert.deepEqual(Array.from(state.rows[2].slice(1)), [21.5, 55.1, -2.5, 99.9]);
  assert.equal(state.flushes, 1);
  assert.equal(state.lockReleases, 1);
  assert.equal(state.lockTimeout, 30000);
});

test('doPost captures the receipt timestamp before waiting on spreadsheet preparation', () => {
  const { api, state } = createHarness({ openDelayMs: 25 });

  const response = json(api.doPost(event()));

  assert.equal(response.ok, true);
  assert.equal(Date.parse(response.timestamp) <= state.openStartedAt, true);
  assert.equal(state.rows[1][0].toISOString(), response.timestamp);
});

test('doPost formats the newest row on every append when the sheet must grow', () => {
  const existing = [[...HEADERS, 'Events'], ['old date', 20, 50, 25, 60, 'Window opened']];
  const { api, state } = createHarness({ rows: existing, maxRows: 2 });
  state.numberFormats.set('2:6', '@');
  const snapshots = [];
  for (let attempt = 0; attempt < 2; attempt += 1) {
    const response = json(api.doPost(event()));
    assert.equal(response.ok, true);
    assert.equal(state.rows[response.row - 1][0].toISOString(), response.timestamp);
    snapshots.push(Array.from({ length: 5 }, (_, column) =>
      state.numberFormats.get(`${response.row}:${column + 1}`)));
  }

  // Both newest rows must be formatted before another request arrives.
  assert.deepEqual(snapshots, [
    ['yyyy-mm-dd hh:mm:ss', '0.0', '0.0', '0.0', '0.0'],
    ['yyyy-mm-dd hh:mm:ss', '0.0', '0.0', '0.0', '0.0'],
  ]);
  assert.deepEqual(state.rows.slice(0, 2), existing);
  assert.equal(state.numberFormats.get('2:6'), '@');
  assert.equal(state.numberFormats.has('3:6'), false);
  assert.equal(state.numberFormats.has('4:6'), false);
});

test('doPost formats the first reading when only the header row fits', () => {
  const { api, state } = createHarness({ maxRows: 1 });

  const response = json(api.doPost(event()));

  assert.equal(response.ok, true);
  assert.equal(response.row, 2);
  assert.equal(state.numberFormats.get('2:1'), 'yyyy-mm-dd hh:mm:ss');
  for (let column = 2; column <= 5; column += 1) {
    assert.equal(state.numberFormats.get(`2:${column}`), '0.0');
  }
});

test('doPost initializes and configures a missing Measurements sheet before appending', () => {
  const { api, state } = createHarness({ sheetExists: false });

  assert.equal(json(api.doPost(event())).ok, true);
  assert.deepEqual(state.rows[0], HEADERS);
  assert.equal(state.rows.length, 2);
  assert.equal(state.timezone, 'Asia/Colombo');
  assert.equal(state.frozenRows, 1);
  for (const row of [2, 1000]) {
    assert.equal(state.numberFormats.get(`${row}:1`), 'yyyy-mm-dd hh:mm:ss');
    for (let column = 2; column <= 5; column += 1) {
      assert.equal(state.numberFormats.get(`${row}:${column}`), '0.0');
    }
  }
  assert.equal(state.numberFormats.has('1:1'), false);
});

test('doPost refuses wrong existing headers and preserves all existing data', () => {
  const rows = [['Timestamp', 'Wrong'], ['keep me', 42]];
  const { api, state } = createHarness({ rows });

  assert.deepEqual(json(api.doPost(event())), { ok: false, error: 'header_mismatch' });
  assert.deepEqual(state.rows, rows);
  assert.equal(state.flushes, 0);
  assert.equal(state.lockReleases, 1);
});

test('doPost returns lock_unavailable without storage access when the script lock cannot be acquired', async (t) => {
  for (const options of [{ lockAcquired: false }, { lockError: true }]) {
    await t.test(JSON.stringify(options), () => {
      const { api, state } = createHarness(options);
      assert.deepEqual(json(api.doPost(event())), {
        ok: false,
        error: 'lock_unavailable',
      });
      assert.equal(state.spreadsheetOpens, 0);
      assert.equal(state.lockReleases, 0);
    });
  }
});

test('doPost hides storage errors and always releases an acquired lock', async (t) => {
  for (const options of [{ openError: true }, { appendError: true }, { flushError: true }]) {
    await t.test(JSON.stringify(options), () => {
      const { api, state } = createHarness(options);
      assert.deepEqual(json(api.doPost(event())), { ok: false, error: 'storage_error' });
      assert.equal(state.lockReleases, 1);
    });
  }
});

test('doPost reports inaccessible properties as configuration_error without locking', () => {
  const { api, state } = createHarness({ propertiesError: true });

  assert.deepEqual(json(api.doPost(event())), {
    ok: false,
    error: 'configuration_error',
  });
  assertNoWrite(state);
});

test('setup creates the header and applies spreadsheet configuration under a lock', () => {
  const { api, state } = createHarness({ sheetExists: false });

  assert.equal(api.setup(), undefined);
  assert.deepEqual(state.rows, [HEADERS]);
  assert.equal(state.timezone, 'Asia/Colombo');
  assert.equal(state.frozenRows, 1);
  for (const row of [2, 1000]) {
    assert.equal(state.numberFormats.get(`${row}:1`), 'yyyy-mm-dd hh:mm:ss');
    for (let column = 2; column <= 5; column += 1) {
      assert.equal(state.numberFormats.get(`${row}:${column}`), '0.0');
    }
  }
  assert.equal(state.numberFormats.has('1:1'), false);
  assert.equal(state.flushes, 1);
  assert.equal(state.lockReleases, 1);
  assert.deepEqual(state.logs, ['Climate logger setup complete: Measurements is ready.']);
});

test('setup preserves existing headers and data while refreshing configuration', () => {
  const rows = [HEADERS, ['old date', 21, 50, 28, 80]];
  const { api, state } = createHarness({ rows });

  assert.equal(api.setup(), undefined);
  assert.deepEqual(state.rows, rows);
  assert.equal(state.timezone, 'Asia/Colombo');
  assert.equal(state.frozenRows, 1);
  assert.equal(state.flushes, 1);
  assert.equal(state.lockReleases, 1);
});

test('setup rejects wrong headers without overwriting existing rows', () => {
  const rows = [['Date', 'Inside'], ['keep', 'this']];
  const { api, state } = createHarness({ rows });

  assert.throws(
    () => api.setup(),
    /Setup failed: Measurements has incompatible headers\./,
  );
  assert.deepEqual(state.rows, rows);
  assert.equal(state.flushes, 0);
  assert.equal(state.lockReleases, 1);
});

test('setup throws visible safe errors for configuration, lock, and storage failures', async (t) => {
  for (const [name, options, expected] of [
    [
      'configuration',
      { properties: { DEVICE_TOKEN: '' } },
      /Setup failed: check SHEET_ID and DEVICE_TOKEN Script Properties\./,
    ],
    [
      'lock',
      { lockAcquired: false },
      /Setup failed: could not acquire the script lock\. Try again\./,
    ],
    [
      'storage',
      { openError: true },
      /Setup failed: could not initialize Measurements\./,
    ],
  ]) {
    await t.test(name, () => {
      const { api, state } = createHarness(options);
      assert.throws(() => api.setup(), expected);
      assert.deepEqual(state.logs, []);
      if (name === 'storage') assert.equal(state.lockReleases, 1);
    });
  }
});
