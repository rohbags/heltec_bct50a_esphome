const $=id=>document.getElementById(id);
let deviceConnected=false,monitoring=false,points=[],lastPointAt=0,lastData=null,wasRunning=false,plotMode='';

let liveGraphMetadata = null;
let liveGraphRecordId = null;
let liveGraphBaseElapsed = 0;
let liveGraphBaseTime = 0;
let liveGraphReady = false;
let liveGraphRestoring = false;

let mainResults = [];
let expandedResultCycles = new Set();
let selectedMainResultId = null;


// Dark/light theme with persistent preference
function applyTheme(theme) {
  document.documentElement.dataset.theme = theme;
  $('themeToggle').textContent = theme === 'dark' ? 'Light mode' : 'Dark mode';
}


function toggleTheme() {
  const current = document.documentElement.dataset.theme || 'light';
  const next = current === 'dark' ? 'light' : 'dark';
  applyTheme(next);
  localStorage.setItem('bct50a-theme', next);
}


// Restore saved theme, defaulting to light mode
applyTheme(localStorage.getItem('bct50a-theme') || 'light');


function fmt(v,n=2){return v===null||v===undefined||!Number.isFinite(Number(v))?'—':Number(v).toFixed(n)}


function hms(s){if(s===null||s===undefined)return '—';s=Math.max(0,Math.floor(Number(s)));let h=Math.floor(s/3600),m=Math.floor((s%3600)/60),x=s%60;return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(x).padStart(2,'0')}


function showTab(tab){
  if (tab === 'single' && $('singleTab').disabled) return;
  if (tab === 'cycle' && $('cycleTab').disabled) return;
  $('singlePanel').classList.toggle('active',tab==='single');
  $('cyclePanel').classList.toggle('active',tab==='cycle');
  $('singleTab').classList.toggle('active',tab==='single');
  $('cycleTab').classList.toggle('active',tab==='cycle');
  $('resultsPanel').classList.toggle('active',tab==='results');
  $('resultsTab').classList.toggle('active',tab==='results');
  
  if (tab === 'results') { loadMainResults(); }
}


let cycleRows = [];
let cycleResults = [];
let cycleRunning = false;
let cyclePaused = false;
let cycleSelectedStep = 1;

const cycleModeNames = {
  cc_charge: 'CC Charge',
  cc_discharge: 'CC Discharge',
  cv_discharge: 'CV Discharge',
  rest: 'Rest',
  loop: 'Loop'
};


function cycleNumber(value, digits = 2) {
  const n = Number(value);
  return Number.isFinite(n) ? n.toFixed(digits) : '—';
}


function updateCycleControls(status) {
  cycleRunning = status === 'running' || status === 'paused';
  cyclePaused = status === 'paused';
  const busy = cycleRunning;
  // Editing buttons and Start
  $('cycleSetBtn').disabled = busy;
  $('cycleDeleteBtn').disabled = busy;
  $('cycleClearBtn').disabled = busy;
  $('cycleStartBtn').disabled = busy;
  // All cycle parameter inputs
  const inputs = [
    'cycleStep',
    'cycleMode',
    'cyclePreset',
    'cycleVoltage',
    'cycleCurrent',
    'cycleEnd'
  ];
  // Restore normal mode-specific input states first.
  updateCycleFields();
  inputs.forEach(id => {
    $(id).disabled = busy;
  });
  // Pause button toggles between Pause and Resume.
  $('cyclePauseBtn').textContent = cyclePaused ? 'Resume' : 'Pause';
  $('cyclePauseBtn').disabled = !busy;
  // Stop is only useful while a cycle is active.
  $('cycleStopBtn').disabled = !busy;
  updateTestTabs();
}


function updateTestTabs() {
  const singleRunning = !!(lastData && lastData.running);
  $('singleTab').disabled = cycleRunning;
  $('cycleTab').disabled = singleRunning && !cycleRunning;
}


function renderCycleRows() {
  const tbody = $('cycleParameterRows');
  tbody.innerHTML = cycleRows.map(r => `
    <tr class="${cycleSelectedStep === r.id ? 'selected' : ''}"
        onclick="selectCycleStep(${r.id})">
      <td>${r.id}</td>
      <td>${cycleModeNames[r.mode]}</td>
      <td>${cycleNumber(r.voltage)}</td>
      <td>${cycleNumber(r.current)}</td>
      <td>${cycleNumber(r.end)}</td>
    </tr>
  `).join('');
  const select = $('cycleStep');
  const maxStep = cycleRows.length + 1;
  const previousValue = cycleSelectedStep;
  select.innerHTML = Array.from(
    { length: maxStep },
    (_, i) => `<option value="${i + 1}">${i + 1}</option>`
  ).join('');
  cycleSelectedStep = Math.min(previousValue, maxStep);
  select.value = String(cycleSelectedStep);
}


function renderCycleResults() {
  $('cycleResultRows').innerHTML = cycleResults.map(r => `
    <tr
      class="${Number(r.id) === selectedCycleResultId ? 'selected' : ''}"
      onclick="selectCycleResult(${Number(r.id)})"
      style="cursor:pointer"
    >
      <td>${r.id}</td>
      <td>${cycleModeNames[r.mode] || r.mode}</td>
      <td>${formatLocalTimestamp(r.start_time)}</td>
      <td>${hms(r.runtime_seconds)}</td>
      <td>${cycleNumber(r.final_capacity_ah, 3)}</td>
      <td>${cycleNumber(r.final_energy_wh, 3)}</td>
      <td><button onclick="event.stopPropagation();saveResultImage(${Number(r.id)})">Save</button></td>
      <td><button onclick="event.stopPropagation();saveResultCsv(${Number(r.id)})">Save</button></td>
    </tr>
  `).join('');
}


function formatLocalTimestamp(timestamp) {
  const seconds = Number(timestamp);
  if (!Number.isFinite(seconds) || seconds <= 0) return '—';
  return new Date(seconds * 1000).toLocaleString('en-AU', {
    year: 'numeric',
    month: 'short',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false
  });
}


let selectedCycleResultId = null;


function selectCycleResult(id) {
  const record = cycleResults.find(r => Number(r.id) === Number(id));
  if (!record) {
    console.warn('Cycle result not found:', id);
    return;
  }
  if (!Array.isArray(record.samples) || !record.samples.length) {
    console.warn('Selected result has no graph samples:', id);
    return;
  }
  selectedCycleResultId = Number(id);
  renderCycleResults();
  const graphPoints = record.samples.map(sample => ({
    t: Number(sample.elapsed_ms),
    v: Number(sample.voltage_v),
    a: Math.abs(Number(sample.current_a))
  })).filter(p =>
    Number.isFinite(p.t) &&
    Number.isFinite(p.v) &&
    Number.isFinite(p.a)
  );
  if (!graphPoints.length) {
    console.warn('No valid graph points for result:', id);
    return;
  }
  // Draw historical result on the existing main graph canvas.
  // The elapsed time axis is relative to the start of this test.
  //drawGraph('plot', graphPoints, true);
  drawGraph('plot', graphPoints, true, {
   start_time: record.start_time,
   end_time: record.end_time,
   mode: record.mode,
   set_current_a: record.set_current_a,
   set_voltage_v: record.set_voltage_v,
   termination_current_a: record.termination_current_a,
   runtime_seconds: record.runtime_seconds,
   final_capacity_ah: record.final_capacity_ah,
   final_energy_wh: record.final_energy_wh
  });
  $('monitorState').textContent = `Viewing result #${record.id}`;
}


async function loadCycleResults() {
  try {
    const response = await fetch('/api/history', { cache: 'no-store' });
    if (!response.ok) {
      throw new Error('History request failed');
    }
    const data = await response.json();
    cycleResults = (data.records || []).filter(record =>
      Number(record.step_cycle) > 0 &&
      record.status !== 'running' &&
      record.status !== 'stopped' &&
      record.status !== 'aborted'
    );
    renderCycleResults();
    if (selectedCycleResultId !== null) {
      const selected = cycleResults.find( r => Number(r.id) === selectedCycleResultId );
      if (selected) { selectCycleResult(selectedCycleResultId); }
      else {
        selectedCycleResultId = null;
        $('cycleResultGraph').hidden = true;
      }
    }
  } catch (error) {
    console.error('Failed to load cycle results:', error);
  }
}


function selectCycleStep(id) {
  cycleSelectedStep = id;
  $('cycleStep').value = String(id);
  loadCycleStep();
}


function loadCycleStep() {
  cycleSelectedStep = Number($('cycleStep').value) || 1;
  const row = cycleRows.find(r => r.id === cycleSelectedStep);
  if (row) {
    // The first step must be a test, never Rest or Loop.
    if (cycleSelectedStep === 1 &&
        (row.mode === 'rest' || row.mode === 'loop')) {
      row.mode = 'cc_charge';
    }
    $('cycleMode').value = row.mode;
    $('cycleVoltage').value = row.voltage;
    $('cycleCurrent').value = row.current;
    $('cycleEnd').value = row.end;
  } else {
    $('cycleMode').value = 'cc_charge';
    $('cycleVoltage').value = '3.65';
    $('cycleCurrent').value = '25';
    $('cycleEnd').value = '0.10';
  }
  updateCycleFields();
  renderCycleRows();
}


function updateCycleFields() {
  const firstStep = Number($('cycleStep').value) === 1;
  $('cycleMode').querySelector('option[value="rest"]').disabled = firstStep;
  $('cycleMode').querySelector('option[value="loop"]').disabled = firstStep;
  // Correct the selection if it is no longer permitted.
  if (firstStep &&
      ($('cycleMode').value === 'rest' || $('cycleMode').value === 'loop')) {
    $('cycleMode').value = 'cc_charge';
  }
  const mode = $('cycleMode').value;
  const voltage = $('cycleVoltage');
  const current = $('cycleCurrent');
  const end = $('cycleEnd');
  voltage.disabled = mode === 'rest' || mode === 'loop';
  current.disabled = mode === 'rest' || mode === 'loop';
  if (mode === 'cc_charge') {
    end.min = '0.01';
    end.step = '0.01';
    $('cycleEndHint').textContent = 'Charge termination (A)';
  } else if (mode === 'rest') {
    end.min = '1';
    end.step = '1';
    $('cycleEndHint').textContent = 'Rest duration (seconds)';
  } else if (mode === 'loop') {
    end.min = '1';
    end.step = '1';
    $('cycleEndHint').textContent = 'Number of repeats';
  } else {
    end.min = '0';
    end.step = '0.01';
    $('cycleEndHint').textContent = 'Not used for this mode';
  }
  if (mode === 'rest' || mode === 'loop') {
    voltage.value = '0';
    current.value = '0';
  }
}


function setCycleStep() {
  if (cycleRunning && !cyclePaused) {
    alert('Pause or stop the cycle before editing steps.');
    return;
  }
  const id = Number($('cycleStep').value);
  const mode = $('cycleMode').value;
  const voltage = Number($('cycleVoltage').value);
  const current = Number($('cycleCurrent').value);
  const end = Number($('cycleEnd').value);
  if (!Number.isInteger(id) || id < 1) {
    alert('Invalid step ID.');
    return;
  }
  if (!Number.isFinite(end) || end < 0) {
    alert('Set End must be zero or greater.');
    return;
  }
  if (mode === 'rest' && (!Number.isInteger(end) || end < 1)) {
    alert('Rest duration must be at least 1 whole second.');
    return;
  }
  if (mode === 'loop' && (!Number.isInteger(end) || end < 1)) {
    alert('Loop count must be at least 1.');
    return;
  }
  if (mode === 'cc_charge' && (voltage < 0.1 || voltage > 5 ||
      current < 0.01 || current > 50 || end < 0.01 || end > 50)) {
    alert('Check charge voltage, current and termination current.');
    return;
  }
  if ((mode === 'cc_discharge' || mode === 'cv_discharge') &&
      (voltage < 0.1 || voltage > 5 || current < 0.01 || current > 50)) {
    alert('Check voltage and current settings.');
    return;
  }
  const row = {
    id,
    mode,
    voltage: mode === 'rest' || mode === 'loop' ? 0 : voltage,
    current: mode === 'rest' || mode === 'loop' ? 0 : current,
    end
  };
  const existing = cycleRows.findIndex(r => r.id === id);
  if (existing >= 0) cycleRows[existing] = row;
  else cycleRows.push(row);
  cycleRows.sort((a, b) => a.id - b.id);
  // Keep IDs sequential to prevent gaps in the execution sequence.
  cycleRows = cycleRows.map((r, i) => ({ ...r, id: i + 1 }));
/*  $('cycleStep').value = String(
    Math.min(id + 1, cycleRows.length + 1)
  );*/
  cycleSelectedStep = id + 1;
  renderCycleRows();
  loadCycleStep();
  $('cycleStatus').textContent = `Step ${id} saved.`;
}


function clearCycle() {
  if (cycleRunning && !cyclePaused) {
    alert('Pause or stop the cycle before clearing it.');
    return;
  }
  if (!confirm('Clear all cycle steps and results?')) return;
  cycleRows = [];
  cycleResults = [];
  renderCycleRows();
  renderCycleResults();
  loadCycleStep();
  $('cycleStatus').textContent = 'Cycle steps and results cleared.';
}


function deleteCycleRow() {
  if (cycleRunning && !cyclePaused) {
    alert('Pause or stop the cycle before deleting steps.');
    return;
  }
  const id = Number($('cycleStep').value);
  const index = cycleRows.findIndex(row => row.id === id);
  if (index < 0) {
    $('cycleStatus').textContent = `Step ${id} has no saved row to delete.`;
    return;
  }
  cycleRows.splice(index, 1);
  // Renumber remaining steps sequentially.
  cycleRows = cycleRows.map((row, i) => ({
    ...row,
    id: i + 1
  }));
  // Select the deleted step's position, or the final remaining step.
  cycleSelectedStep = Math.min(id, cycleRows.length + 1);
  renderCycleRows();
  loadCycleStep();
  $('cycleStatus').textContent = `Step ${id} deleted.`;
}


function loadCyclePreset() {
  const preset = $('cyclePreset').value;
  // Selecting None does not modify the current settings.
  if (preset === 'none') return;
  if (cycleRunning && !cyclePaused) {
    alert('Pause or stop the cycle before loading a preset.');
    $('cyclePreset').value = 'none';
    return;
  }
  if (cycleRows.length > 0 &&
      !confirm('Replace all saved cycle steps with this preset?')) {
    $('cyclePreset').value = 'none';
    return;
  }
  const chargeVoltage = preset === 'lifepo4' ? 3.65 : 4.20;
  const testCurrent = preset === 'lifepo4' ? 50 : 1;
  cycleRows = [
    {
      id: 1,
      mode: 'cc_charge',
      voltage: chargeVoltage,
      current: testCurrent,
      end: 0.10
    },
    {
      id: 2,
      mode: 'rest',
      voltage: 0,
      current: 0,
      end: 30
    },
    {
      id: 3,
      mode: 'cc_discharge',
      voltage: 2.50,
      current: testCurrent,
      end: 0
    }
  ];
  cycleResults = [];
  cycleSelectedStep = 1;
  renderCycleRows();
  renderCycleResults();
  loadCycleStep();
  $('cycleStatus').textContent =
    `${preset === 'lifepo4' ? 'LiFePO4' : 'Li-ion'} preset loaded.`;
  // Allow the same preset to be selected again later.
  $('cyclePreset').value = 'none';
}


async function sendCycleCommand(action) {
  const payload = {
    action,
    steps: cycleRows,
    step: Number($('cycleStep').value)
  };
  try {
    const response = await fetch('/api/cycle', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    const result = await response.json();
    if (!response.ok || !result.ok) {
      throw new Error(result.error || 'Cycle command rejected');
    }
    if (result.results) {
      cycleResults = result.results;
      renderCycleResults();
    }
    if (result.current_step) {
      $('cycleStep').value = String(result.current_step);
      loadCycleStep();
    }
    if (result.status) {
      $('cycleStatus').textContent = result.status;
    }
  } catch (error) {
    $('cycleStatus').textContent = 'Cycle command failed: ' + error.message;
  }
}


function startCycle() {
  if (!cycleRows.length) {
    alert('Add at least one cycle step first.');
    return;
  }
  const ids = cycleRows.map(r => r.id);
  if (ids.some((id, i) => id !== i + 1)) {
    alert('Cycle step IDs must be sequential.');
    return;
  }
  sendCycleCommand('start');
}


function pauseCycle() {
  sendCycleCommand(cyclePaused ? 'resume' : 'pause');
}


function stopCycle() {
  sendCycleCommand('stop');
}


let cycleShowingResults = false;


function toggleCycleTable() {
  cycleShowingResults = !cycleShowingResults;
  $('cycleSettingsView').hidden = cycleShowingResults;
  $('cycleResultsView').hidden = !cycleShowingResults;
  $('cycleTableTitle').textContent = cycleShowingResults ? 'Cycle results' : 'Parameter setting';
  $('cycleTableToggle').textContent = cycleShowingResults ? 'Settings' : 'Results';
  if (cycleShowingResults) {
    loadCycleResults();
  }
}


// main results tab
async function loadMainResults() {
  try {
    const response = await fetch('/api/history', { cache: 'no-store' });
    if (!response.ok) {
      throw new Error('History request failed');
    }
    const data = await response.json();
    // Only show finished results.
    const records = (data.records || []).filter(record =>
      record.status !== 'running' &&
      record.status !== 'stopped' &&
      record.status !== 'aborted'
    );
    // Ensure records are processed chronologically.
    records.sort((a, b) => Number(a.id) - Number(b.id));
    mainResults = [];
    let currentCycle = null;
    records.forEach(record => {
      const step = Number(record.step_cycle ?? record.cycle_step ?? 0);
      if (step === 0) {
        // Standalone single test.
        currentCycle = null;
        mainResults.push({
          type: 'single',
          record
        });
      } else if (step === 1) {
        // Beginning of a new cycle.
        currentCycle = {
          type: 'cycle',
          id: Number(record.id),
          steps: [record]
        };
        mainResults.push(currentCycle);
      } else {
        // Subsequent step in the current cycle.
        if (!currentCycle) {
          // Defensive fallback if the first retained step is > 1.
          currentCycle = {
            type: 'cycle',
            id: Number(record.id),
            steps: []
          };
          mainResults.push(currentCycle);
        }
        currentCycle.steps.push(record);
      }
    });
    renderMainResults();
  } catch (error) {
    console.error('Failed to load main results:', error);
  }
}


async function deleteResult(id) {
  if (!confirm(`Delete history record #${id}? This cannot be undone.`)) { return; }
  try {
    const response = await fetch( `/api/history/delete?id=${encodeURIComponent(id)}`, { method: 'POST' } );
    if (!response.ok) {
      const message = await response.text();
      throw new Error(message || `HTTP ${response.status}`);
    }
    const result = await response.json();
    console.log(`Deleted history #${id} from ${result.source}`);
    // Reload history and rebuild the Results tab.
    // A reload also ensures cycle grouping and selection are correct.
//    window.location.reload();
    await loadMainResults();
  } catch (error) {
    console.error('Delete history failed:', error);
    alert(`Unable to delete history #${id}: ${error.message}`);
  }
}


function renderMainResults() {
  let html = '';
  mainResults.forEach(entry => {
    if (entry.type === 'single') {
      const r = entry.record;
      const id = Number(r.id);
      html += `
        <tr
          class="${id === selectedMainResultId ? 'selected' : ''}"
          onclick="selectMainResult(${id})"
          style="cursor:pointer"
        >
          <td>${id}</td>
          <td>${cycleModeNames[r.mode] || r.mode}</td>
          <td>${formatLocalTimestamp(r.start_time)}</td>
          <td>${hms(r.runtime_seconds)}</td>
          <td>${cycleNumber(r.final_capacity_ah, 3)}</td>
          <td>${cycleNumber(r.final_energy_wh, 3)}</td>
          <td>${r.source || 'psram'}</td>
          <td>
            <button onclick="event.stopPropagation();saveResultImage(${id})">Save</button>
          </td>
          <td>
            <button onclick="event.stopPropagation();saveResultCsv(${id})">Save</button>
          </td>
          <td>
            <button onclick="event.stopPropagation();deleteResult(${id})">Delete</button>
          </td>
        </tr>
      `;
    } else if (entry.type === 'cycle') {
      const first = entry.steps[0];
      const id = entry.id;
      const expanded = expandedResultCycles.has(id);
      const totalRuntime = entry.steps.reduce( (total, r) => total + (Number(r.runtime_seconds) || 0), 0 );
      const sources = [...new Set(entry.steps.map(r => r.source || 'psram'))];
      const cycleSource = sources.length === 1 ? sources[0] : 'Mixed';
      html += `
        <tr class="result-cycle-row" onclick="toggleResultCycle(${id})" style="cursor:pointer" >
          <td>${id}</td>
          <td>${expanded ? '▼' : '▶'} Cycle (${entry.steps.length} steps)</td>
          <td>${formatLocalTimestamp(first.start_time)}</td>
          <td>${hms(totalRuntime)}</td>
          <td></td>
          <td></td>
          <td>${cycleSource}</td>
          <td></td>
          <td></td>
          <td></td>
        </tr>
      `;
      if (expanded) {
        entry.steps.forEach(r => {
          const stepId = Number(r.id);
          const step = Number(r.step_cycle ?? r.cycle_step ?? 0);
          html += `
            <tr
              class="result-step-row ${stepId === selectedMainResultId ? 'selected' : ''}"
              onclick="selectMainResult(${stepId})" style="cursor:pointer" >
              <td>${stepId}</td>
              <td>&nbsp;&nbsp; Step ${step} — ${cycleModeNames[r.mode] || r.mode}</td>
              <td>${formatLocalTimestamp(r.start_time)}</td>
              <td>${hms(r.runtime_seconds)}</td>
              <td>${cycleNumber(r.final_capacity_ah, 3)}</td>
              <td>${cycleNumber(r.final_energy_wh, 3)}</td>
              <td>${r.source || 'psram'}</td>
              <td>
                <button onclick="event.stopPropagation();saveResultImage(${stepId})">Save</button>
              </td>
              <td>
                <button onclick="event.stopPropagation();saveResultCsv(${stepId})">Save</button>
              </td>
              <td>
                <button onclick="event.stopPropagation();deleteResult(${stepId})">Delete</button>
              </td>
            </tr>
          `;
        });
      }
    }
  });
  $('resultsRows').innerHTML = html;
}



function toggleResultCycle(id) {
  if (expandedResultCycles.has(id)) {
    expandedResultCycles.delete(id);
  } else {
    expandedResultCycles.add(id);
  }
  renderMainResults();
}


async function selectMainResult(id) {
  try {
    const record = await fetchHistoryRecord(id);
    const graphPoints = (record.samples || []).map(sample => ({
      t: Number(sample.elapsed_ms),
      v: Number(sample.voltage_v),
      a: Math.abs(Number(sample.current_a))
    }));
    const metadata = {
      mode: record.mode,
      start_time: record.start_time,
      end_time: record.end_time,
      set_current_a: record.set_current_a,
      set_voltage_v: record.set_voltage_v,
      termination_current_a: record.termination_current_a,
      runtime_seconds: record.runtime_seconds,
      final_capacity_ah: record.final_capacity_ah,
      final_energy_wh: record.final_energy_wh
    };
    selectedMainResultId = Number(id);
    drawGraph('plot', graphPoints, true, metadata);
    renderMainResults();
    console.log(
      `History record ${id}: loaded ${graphPoints.length} of ${record.sample_count} samples`
    );
  } catch (error) {
    console.error('Failed to display selected result:', error);
  }
}


async function fetchHistoryRecord(id) {
  const pageSize = 256;
  let offset = 0;
  let record = null;
  const allSamples = [];
  const progress = $('historyLoadProgress');
  const progressText = $('historyLoadProgressText');
  const progressBar = $('historyLoadProgressBar');
  function updateProgress(fetched, total) {
    const percent = total > 0 ? Math.min(100, (fetched / total) * 100) : 100;
    progress.hidden = false;
    progressText.textContent = `Loading history: ${fetched} / ${total}`;
    progressBar.style.width = `${percent}%`;
  }
  try {
    while (true) {
      const response = await fetch(
        `/api/history?id=${encodeURIComponent(id)}&offset=${offset}&limit=${pageSize}`,
        { cache: 'no-store' }
      );
      if (!response.ok) {
        throw new Error(`History request failed: HTTP ${response.status}`);
      }
      const data = await response.json();
      const pageRecord = data.records?.[0];
      if (!pageRecord) {
        throw new Error(`History record ${id} not found`);
      }
      if (!record) {
        record = { ...pageRecord };
      }
      const samples = pageRecord.samples || [];
      allSamples.push(...samples);
      updateProgress(allSamples.length, Number(record.sample_count));
      const nextOffset = Number(pageRecord.samples_offset ?? offset) + samples.length;
      if (
        samples.length === 0 ||
        nextOffset >= Number(record.sample_count)
      ) {
        break;
      }
      if (nextOffset <= offset) {
        throw new Error('History pagination made no progress');
      }
      offset = nextOffset;
    }
    record.samples = allSamples;
    record.samples_offset = 0;
    if (allSamples.length !== Number(record.sample_count)) {
      console.warn(
        `History record ${id}: received ${allSamples.length} of ${record.sample_count} samples`
      );
    }
    return record;
  } finally {
    progress.hidden = true;
    progressBar.style.width = '0%';
  }
}


function resultFilename(record, extension) {
  const date = new Date(Number(record.start_time) * 1000);
  const pad = value => String(value).padStart(2, '0');
  const timestamp =
    date.getFullYear() +
    pad(date.getMonth() + 1) +
    pad(date.getDate()) + '_' +
    pad(date.getHours()) +
    pad(date.getMinutes()) +
    pad(date.getSeconds());
  return `bct50a_${timestamp}.${extension}`;
}


function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}


async function saveResultCsv(id) {
  try {
    const record = await fetchHistoryRecord(id);
    const csvEscape = value => {
      const text = value == null ? '' : String(value);
      return `"${text.replace(/"/g, '""')}"`;
    };
    const metadata = [
      ['Record ID', record.id],
      ['Mode', record.mode],
      ['Status', record.status],
      ['Start time', formatLocalTimestamp(record.start_time)],
      ['End time', formatLocalTimestamp(record.end_time)],
      ['Set current (A)', record.set_current_a],
      ['Set voltage (V)', record.set_voltage_v],
      ['Termination current (A)', record.termination_current_a],
      ['Runtime (seconds)', record.runtime_seconds],
      ['Final capacity (Ah)', record.final_capacity_ah],
      ['Final energy (Wh)', record.final_energy_wh],
      ['Cycle step', record.step_cycle],
      ['Sample count', record.sample_count]
    ];
    const rows = metadata.map(row => row.map(csvEscape).join(','));
    rows.push('');
    rows.push([
      'Elapsed time (ms)',
      'Voltage (V)',
      'Current (A)',
      'Capacity (Ah)',
      'Energy (Wh)'
    ].map(csvEscape).join(','));
    (record.samples || []).forEach(sample => {
      rows.push([
        sample.elapsed_ms,
        sample.voltage_v,
        sample.current_a,
        sample.capacity_ah,
        sample.energy_wh
      ].map(csvEscape).join(','));
    });
    const csv = '\uFEFF' + rows.join('\r\n');
    const blob = new Blob([csv], { type: 'text/csv;charset=utf-8' });
    downloadBlob(blob, resultFilename(record, 'csv'));
  } catch (error) {
    console.error('Failed to export result CSV:', error);
    alert('Unable to export CSV: ' + error.message);
  }
}


async function saveResultImage(id) {
  let canvas = null;
  try {
    const record = await fetchHistoryRecord(id);
    const graphPoints = (record.samples || []).map(sample => ({
      t: Number(sample.elapsed_ms),
      v: Number(sample.voltage_v),
      a: Math.abs(Number(sample.current_a))
    }));
    const metadata = {
      mode: record.mode,
      start_time: record.start_time,
      end_time: record.end_time,
      set_current_a: record.set_current_a,
      set_voltage_v: record.set_voltage_v,
      termination_current_a: record.termination_current_a,
      runtime_seconds: record.runtime_seconds,
      final_capacity_ah: record.final_capacity_ah,
      final_energy_wh: record.final_energy_wh
    };
    // Use the existing graph's dimensions.
    const plot = $('plot');
    const rect = plot.getBoundingClientRect();
    if (!rect.width || !rect.height) {
      throw new Error('Graph dimensions are unavailable');
    }
    // Create an off-screen canvas with matching dimensions.
    canvas = document.createElement('canvas');
    canvas.id = 'resultExportCanvas';
    canvas.className = plot.className;
    canvas.style.position = 'fixed';
    canvas.style.left = '-10000px';
    canvas.style.top = '0';
    canvas.style.width = rect.width + 'px';
    canvas.style.height = rect.height + 'px';
    document.body.appendChild(canvas);
    // Render the historical graph without changing the visible graph.
    drawGraph('resultExportCanvas', graphPoints, true, metadata);
    // Export the rendered canvas.
    const blob = await new Promise((resolve, reject) => {
      canvas.toBlob(result => {
        if (result) resolve(result);
        else reject(new Error('PNG generation failed'));
      }, 'image/png');
    });
    downloadBlob(blob, resultFilename(record, 'png'));
  } catch (error) {
    console.error('Failed to export result image:', error);
    alert('Unable to export PNG: ' + error.message);
  } finally {
    if (canvas) {
      canvas.remove();
    }
  }
}


function updateControls(modeChanged=false){
  const charge=$('mode').value==='cc_charge';
  $('termCurrent').disabled=!charge;
  if(modeChanged)applyPreset(false);
  updateButtons()
}


function applyPreset(){
  const preset=$('preset').value,charge=$('mode').value==='cc_charge';
  if(preset==='none') return;
  if(preset==='lifepo4'){
    $('setCurrent').value='50.00';
    $('setVoltage').value=charge?'3.65':'2.50'
  }
  else if(preset==='liion'){
    $('setVoltage').value=charge?'4.20':'2.50'
    $('setCurrent').value='1.00';
  }
}


async function api(action){
  const c=Number($('setCurrent').value),v=Number($('setVoltage').value),t=Number($('termCurrent').value);
  if(action.startsWith('start_')&&(!Number.isFinite(c)||c<.01||c>50||!Number.isFinite(v)||v<.1||v>5)){
    alert('Current must be 0.01–50 A and voltage limit 0.1–5.0 V.');
    return
  }
  const q=new URLSearchParams({action,current:String(c),voltage:String(v),termination_current:String(t)});
  try{
    const r=await fetch('/api/cmd?'+q.toString()),j=await r.json();
    if(!j.ok)alert(j.error||'Command rejected');
    await refresh()
    }
  catch(e){alert('Could not reach controller: '+e)}
}


function startSelected(){
  api('start_'+$('mode').value);
}


async function BtnCommand(action){
  if(action==='clear'){
    monitoring=false;
    $('monitorState').textContent='Monitoring stopped';
    await api(action);points=[];
    lastPointAt=0;
    drawGraph();
    updateButtons();
    return
  }
  await api(action)
}


function toggleConnection(){
  api(deviceConnected?'disconnect':'connect');
}


function toggleMonitor(){
  if(lastData&&lastData.running) return;
  monitoring=!monitoring;
  if(monitoring){
    points=[];
    lastPointAt=0;
    plotMode='monitor';
    $('monitorState').textContent='Monitoring live data';
  }else{
    $('monitorState').textContent='Monitoring stopped';
  }
  updateButtons();
  drawGraph()
}


function clearGraph(){
  if((lastData&&lastData.running)||monitoring) return;
  points=[];
  lastPointAt=0;
  drawGraph();
  updateButtons();
}


function saveGraph(){
  if(!points.length||(lastData&&lastData.running)||monitoring) return;
  const rows=['Local time,Timestamp (Unix ms),Voltage (V),Current (A)'];
  for(const p of points){
    const time=new Date(p.t).toLocaleString();
    rows.push(['"'+time.replaceAll('"','""')+'"',p.t,p.v.toFixed(4),p.a.toFixed(4)].join(','))
  }
  const blob=new Blob([rows.join('\r\n')],{type:'text/csv;charset=utf-8;'}),url=URL.createObjectURL(blob),link=document.createElement('a');
  link.href=url;
  link.download='bct50a-graph.csv';
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}


function saveGraphImage() {
  const canvas = $('plot');
  // Local datetime: YYYYMMDD_HHMMSS
  const now = new Date();
  const pad = n => String(n).padStart(2, '0');
  const datetime =
    now.getFullYear() +
    pad(now.getMonth() + 1) +
    pad(now.getDate()) + '_' +
    pad(now.getHours()) +
    pad(now.getMinutes()) +
    pad(now.getSeconds());
  const link = document.createElement('a');
  link.download = `bct50a_${datetime}.png`;
  link.href = canvas.toDataURL('image/png');
  document.body.appendChild(link);
  link.click();
  link.remove();
}


function updateButtons(){
  const running=!!(lastData&&lastData.running),state=String(lastData?.state||'').toLowerCase(),ccRunning=running&&state.includes('cc discharge');
  $('adjustBtn').disabled=!ccRunning;
  $('connectBtn').textContent=deviceConnected?'Disconnect':'Connect';
  $('monitorBtn').disabled=running;
  $('monitorBtn').textContent=monitoring?'Stop Monitor':'Monitor';
  const plotting=running||monitoring;
  $('saveGraphBtn').disabled=plotting||points.length===0;
  $('clearGraphBtn').disabled=plotting;
  $('connectBtn').disabled=running;
  $('startBtn').disabled=running;
  $('pauseBtn').disabled=!running;
  $('clearBtn').disabled=!running;
  updateTestTabs();
}


function updateDashboardClock(d) {
  const timestamp = Number(d.unix_time);
  if (Number.isFinite(timestamp) && timestamp > 0) {
    $('dashboardTime').textContent =
      new Date(timestamp * 1000).toLocaleString('en-AU', {
        year: 'numeric',
        month: 'short',
        day: '2-digit',
        hour: '2-digit',
        minute: '2-digit',
        second: '2-digit',
        hour12: false
      });
  } else {
    $('dashboardTime').textContent = 'Time unavailable';
  }
  $('dashboardTimeSource').textContent = '[' + (d.time_source || 'none').toUpperCase() + ']';
}


function updateWifiStatus(d) {
  const wifi = d.wifi || {};
  const bars = $('wifiBars');
  const ssid = $('wifiSsid');
  const rssi = $('wifiRssi');
  const status = $('wifiStatus');
  if (!wifi.connected) {
    bars.className = 'wifi-bars disconnected';
    ssid.textContent = 'Wi-Fi disconnected';
    rssi.textContent = '';
    status.title = 'Not connected to a Wi-Fi network';
    return;
  }
  const signal = Number(wifi.rssi);
  let level = 1;
  if (signal >= -55) level = 4;
  else if (signal >= -67) level = 3;
  else if (signal >= -75) level = 2;
  bars.className = 'wifi-bars level-' + level;
  ssid.textContent = wifi.ssid || 'Wi-Fi connected';
  rssi.textContent = Number.isFinite(signal) ? signal + ' dBm' : '';
  status.title =
      'Wi-Fi: ' + (wifi.ssid || 'Connected') +
      '\nSignal: ' + signal + ' dBm' +
      '\nChannel: ' + (wifi.channel || 'Unknown');
}


async function refresh() {
  try {
    const r = await fetch('/api/status', { cache: 'no-store' });
    const d = await r.json();
    lastData = d;
    const cpuTemp = d.cpu_temp;
    $('cpuTemp').textContent = cpuTemp !== null && cpuTemp !== undefined && Number.isFinite(Number(cpuTemp)) ? Number(cpuTemp).toFixed(1) + '°C' : '--.-°C';
    updateDashboardClock(d);
    updateWifiStatus(d);
    updateStorageDisplay(d);
    updateCycleControls(d.cycle_status || 'idle');
    for (const [id, val, n] of [
      ['voltage', d.voltage, 3],
      ['current', d.current, 2],
      ['ah', d.ah, 2],
      ['wh', d.wh, 2],
      ['echoCurrent', d.set_current, 2],
      ['echoVoltage', d.set_voltage, 2]
    ]) {
      $(id).textContent = fmt(val, n);
    }
    $('runtime').textContent = hms(d.runtime);
    $('state').textContent = d.state || 'Unknown';
    $('state').className = 'status ' + (d.running ? 'run' : '');
    deviceConnected = !!d.connected;
    $('model').textContent = (d.product || '—') + (d.version ? ' ' + d.version : '');
    $('echoTerm').textContent = $('mode').value === 'cc_charge' ? fmt(d.end_value, 2) + ' A' : 'n/a';
    $('link').textContent = 'Frames: ' + d.valid_frames + ' valid · ' + d.bad_frames + ' rejected';
    const running = !!d.running;
    // A newly started test gets a fresh graph.
    if (running && !wasRunning) {
      monitoring = false;
      points = [];
      lastPointAt = 0;
      liveGraphMetadata = null;
      liveGraphRecordId = null;
      liveGraphReady = false;
      plotMode = 'test';
      $('monitorState').textContent = 'Recording test';
    }
    else if (!running && wasRunning) {
      $('monitorState').textContent = 'Test recording stopped';
      if (liveGraphRecordId !== null) { finaliseLiveGraph(liveGraphRecordId); }
    }
    wasRunning = running;
    // Recover the running test if necessary.
    // This also handles a browser refresh during a test.
    if (running && !liveGraphReady && !liveGraphRestoring) {
      await restoreLiveGraph();
    }
    const shouldPlot = running || monitoring;
    const voltage = Number(d.voltage);
    const current = Number(d.current);
    if (running && liveGraphReady && liveGraphMetadata) {
      // Update live measurements and report metadata from api/status.
      liveGraphMetadata.voltage_v = voltage;
      liveGraphMetadata.current_a = current;
      liveGraphMetadata.runtime_seconds = Number(d.runtime);
      liveGraphMetadata.final_capacity_ah = Number(d.ah);
      liveGraphMetadata.final_energy_wh = Number(d.wh);
      // Append live samples using the restored elapsed-time reference.
      if (
        Number.isFinite(voltage) &&
        Number.isFinite(current)
      ) {
        const now = Date.now();

        if (!lastPointAt || now - lastPointAt >= 1000) {
          const elapsed =
            liveGraphBaseElapsed +
            (performance.now() - liveGraphBaseTime);

          points.push({
            t: elapsed,
            v: Math.max(0, voltage),
            a: Math.abs(current)
          });
          lastPointAt = now;
          if (points.length > 20000) {
            points.shift();
          }
        }
      }
      // Redraw so the report metadata remains current.
      drawGraph('plot', points, true, liveGraphMetadata);
    } else if (
      shouldPlot &&
      Number.isFinite(voltage) &&
      Number.isFinite(current)
    ) {
      // Existing monitor-only plotting behaviour.
      const now = Date.now();
      if (!lastPointAt || now - lastPointAt >= 1000) {
        points.push({
          t: now,
          v: Math.max(0, voltage),
          a: Math.abs(current)
        });
        lastPointAt = now;
        if (points.length > 20000) {
          points.shift();
        }
        drawGraph();
      }
    }
    updateButtons();
  } catch (e) {
//    $('link').textContent = 'Dashboard API unavailable';
    console.error('Dashboard refresh failed:', e);
    $('link').textContent = 'Dashboard refresh error: ' + (e?.message || e);
  }
}



async function finaliseLiveGraph(id) {
  try {
    const record = await fetchHistoryRecord(id);
    if (!record) {
      console.warn('Completed history record not found:', id);
      return;
    }
    // Use the definitive completed record, including its final sample.
    if (Array.isArray(record.samples)) {
      points = record.samples.map(sample => ({
        t: Number(sample.elapsed_ms),
        v: Number(sample.voltage_v),
        a: Math.abs(Number(sample.current_a))
      }));
    }
    liveGraphMetadata = {
      ...liveGraphMetadata,
      mode: record.mode,
      start_time: record.start_time,
      end_time: record.end_time,
      set_current_a: record.set_current_a,
      set_voltage_v: record.set_voltage_v,
      termination_current_a: record.termination_current_a,
      runtime_seconds: record.runtime_seconds,
      final_capacity_ah: record.final_capacity_ah,
      final_energy_wh: record.final_energy_wh
    };
    drawGraph('plot', points, true, liveGraphMetadata);
  } catch (error) {
    console.error('Unable to finalise live graph:', error);
  }
}



async function restoreLiveGraph() {
  if (liveGraphRestoring) return;
  liveGraphRestoring = true;
  try {
    const record = await fetchHistoryRecord(0);
    if (!record) { throw new Error('No running history record found'); }
    // Restore the historical samples.
    points = (record.samples || []).map(sample => ({
      t: Number(sample.elapsed_ms),
      v: Number(sample.voltage_v),
      a: Math.abs(Number(sample.current_a))
    })).filter(p =>
      Number.isFinite(p.t) &&
      Number.isFinite(p.v) &&
      Number.isFinite(p.a)
    );
    // Establish the time reference for subsequent live samples.
    liveGraphBaseElapsed = points.length ? points[points.length - 1].t : 0;
    liveGraphBaseTime = performance.now();
    lastPointAt = Date.now();
    liveGraphRecordId = Number(record.id);
    // Restore the report metadata.
    liveGraphMetadata = {
      mode: record.mode,
      start_time: record.start_time,
      end_time: record.end_time,
      set_current_a: Number(record.set_current_a),
      set_voltage_v: Number(record.set_voltage_v),
      termination_current_a: Number(record.termination_current_a),
      runtime_seconds: Number(record.runtime_seconds),
      final_capacity_ah: Number(record.final_capacity_ah),
      final_energy_wh: Number(record.final_energy_wh),
      voltage_v: null,
      current_a: null
    };
    liveGraphReady = true;
    $('monitorState').textContent = 'Recording test';
    drawGraph('plot', points, true, liveGraphMetadata);
    console.log( `Restored live graph: record ${record.id}, ${points.length} samples` );
  } catch (error) {
    console.error('Live graph restoration failed:', error);
  } finally {
    liveGraphRestoring = false;
  }
}


function drawGraph(canvasId = 'plot', graphPoints = points, elapsedAxis = false, metadata = {}) {
  const canvas = $(canvasId);
  if (!canvas) return;
  graphPoints = Array.isArray(graphPoints) ? graphPoints : [];
  const plotPoints = graphPoints;
  // Always reserve the same space for the report header.
  const headerHeight = 55;
  const headerExtra = headerHeight - 50;
  // Preserve the original canvas height so repeated redraws
  // don't progressively increase its size.
  if (!canvas.dataset.baseHeight) {
    canvas.dataset.baseHeight = canvas.getBoundingClientRect().height;
  }
  const baseHeight = Number(canvas.dataset.baseHeight);
  canvas.style.height = `${baseHeight + headerExtra}px`;
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  if (!rect.width || !rect.height) return;
  canvas.width = Math.round(rect.width * dpr);
  canvas.height = Math.round(rect.height * dpr);
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const W = rect.width;
  const H = rect.height;
  const left = 48;
  const right = 48;
  const top = headerHeight;
  const bottom = 36;
  const pw = W - left - right;
  const ph = H - top - bottom;
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, W, H);
  // ----- Report header -----
  const modeName = metadata.mode ? (cycleModeNames[metadata.mode] || metadata.mode) : '—';
  const startTime = metadata.start_time ? formatLocalTimestamp(metadata.start_time) : '—';
  const endTime = metadata.end_time ? formatLocalTimestamp(metadata.end_time) : '—';
  const runtime = metadata.runtime_seconds != null ? hms(metadata.runtime_seconds) : '—';
  const fmt = (value, decimals = 3) => {
    const n = Number(value);
    return Number.isFinite(n) ? n.toFixed(decimals) : '—';
  };
  ctx.fillStyle = '#222';
  ctx.textBaseline = 'middle';
  // First line: title on left, timing information on right.
  ctx.font = 'bold 13px sans-serif';
  ctx.textAlign = 'left';
  ctx.fillText('BCT50A TEST REPORT', left, 15);
  ctx.font = '11px sans-serif';
  ctx.textAlign = 'right';
  const fields1 = [
    `Mode: ${modeName}`, `Start: ${startTime}`, `End: ${endTime}`, `Runtime: ${runtime}`
  ];
  const colWidth = pw / 5;
  fields1.forEach((text, i) => {
    ctx.fillText(text, (left + 285) + i * colWidth, 15);
  });
  // Second line: test settings and final results.
  ctx.font = '11px sans-serif';
  ctx.textAlign = 'left';
  const metadataY = 31;
//  const colWidth = pw / 5;
  const fields = [
    `Set voltage: ${fmt(metadata.set_voltage_v)} V`,
    `Set current: ${fmt(metadata.set_current_a)} A`,
    `Set end: ${fmt(metadata.termination_current_a)} A`,
    `Capacity: ${fmt(metadata.final_capacity_ah)} Ah`,
    `Energy: ${fmt(metadata.final_energy_wh)} Wh`
  ];
  fields.forEach((text, i) => {
    ctx.fillText(text, left + i * colWidth, metadataY);
  });
  // Third line: graph legend.
  const legendY = 45;
  ctx.font = 'bold 12px sans-serif';
  ctx.strokeStyle = '#1769d2';
  ctx.lineWidth = 2.5;
  ctx.beginPath();
  ctx.moveTo(left, legendY);
  ctx.lineTo(left + 20, legendY);
  ctx.stroke();
  ctx.fillStyle = '#222';
  ctx.textAlign = 'left';
  ctx.fillText('Voltage (V)', left + 26, legendY);
  const currentLegendX = left + 120;
  ctx.strokeStyle = '#d52b2b';
  ctx.beginPath();
  ctx.moveTo(currentLegendX, legendY);
  ctx.lineTo(currentLegendX + 20, legendY);
  ctx.stroke();
  ctx.fillStyle = '#222';
  ctx.fillText('Current (A)', currentLegendX + 26, legendY);
  // Divider between report header and graph.
//  ctx.beginPath();
//  ctx.strokeStyle = '#ccc';
//  ctx.lineWidth = 2;
//  ctx.moveTo(left, 55);
//  ctx.lineTo(W - right, 55);
//  ctx.stroke();
  // Voltage axis: 0.5 V major divisions, 0.1 V minor divisions.
  const maxV = Math.max(0.1, ...plotPoints.map(p => p.v));
  const vMax = Math.max(0.5, Math.ceil(maxV * 1.10 * 2) / 2);
  // Current axis: clean 0.5 A divisions with headroom.
  const maxA = Math.max(0.1, ...plotPoints.map(p => p.a));
  const aMax = Math.max(0.5, Math.ceil(maxA * 1.10 * 2) / 2);
  const t0 = plotPoints.length ? plotPoints[0].t : Date.now();
  const tLast = plotPoints.length > 1 ? plotPoints[plotPoints.length - 1].t : t0 + 1000;
  const duration = Math.max(1000, tLast - t0);
  ctx.font = '10px sans-serif';
  ctx.textBaseline = 'middle';
  // ----- Horizontal voltage grid -----
  for (let i = 0; i <= Math.round(vMax * 10); i++) {
    const v = i / 10;
    if (v > vMax + 0.0001) continue;
    const y = top + ph - (v / vMax) * ph;
    const isMajor = i % 5 === 0;
    ctx.beginPath();
    ctx.strokeStyle = isMajor ? '#a8a8a8' : '#d8d8d8';
    ctx.lineWidth = isMajor ? 1.4 : 0.7;
    ctx.moveTo(left, y);
    ctx.lineTo(left + pw, y);
    ctx.stroke();
    // Left-side tick marker for every 0.1 V.
    ctx.beginPath();
    ctx.strokeStyle = isMajor ? '#666' : '#999';
    ctx.lineWidth = isMajor ? 1.4 : 0.8;
    ctx.moveTo(left - (isMajor ? 6 : 3), y);
    ctx.lineTo(left, y);
    ctx.stroke();
    // Numbers only at 0.5 V intervals.
    if (isMajor) {
      ctx.fillStyle = '#222';
      ctx.textAlign = 'right';
      ctx.fillText(v.toFixed(1), left - 9, y);
    }
  }
  // ----- Time grid -----
  const second = 1000;
  const minute = 60 * second;
  // Use shorter intervals for short tests.
  let timeMinor;
  if (duration <= 2 * minute) {
    timeMinor = 10 * second;
  } else if (duration <= 5 * minute) {
    timeMinor = 30 * second;
  } else {
    timeMinor = minute;
  }
  const timeMajor = duration <= 15 * minute ? timeMinor : 5 * minute;
  for (let offset = 0; offset <= duration + timeMinor / 2; offset += timeMinor) {
    const x = left + (offset / duration) * pw;
    const isMajor = offset % timeMajor === 0;
    ctx.beginPath();
    ctx.strokeStyle = isMajor ? '#b0b0b0' : '#d8d8d8';
    ctx.lineWidth = isMajor ? 1.2 : 0.7;
    ctx.moveTo(x, top);
    ctx.lineTo(x, top + ph);
    ctx.stroke();
    if (isMajor && offset <= duration) {
      let label;
      if (elapsedAxis) {
        const totalSeconds = Math.floor(offset / second);
        const hours = Math.floor(totalSeconds / 3600);
        const minutes = Math.floor((totalSeconds % 3600) / 60);
        const seconds = totalSeconds % 60;
        label = hours > 0
          ? `${hours}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`
          : `${minutes}:${String(seconds).padStart(2, '0')}`;
      } else {
        const dt = new Date(t0 + offset);
        label = duration < minute
          ? dt.toLocaleTimeString([], {
              hour: '2-digit',
              minute: '2-digit',
              second: '2-digit',
              hour12: false
            })
          : dt.toLocaleTimeString([], {
              hour: '2-digit',
              minute: '2-digit',
              hour12: false
            });
      }
      ctx.fillStyle = '#222';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'alphabetic';
      // Keep labels inside the plot's horizontal bounds.
      const labelX = Math.max(
        left + 23,
        Math.min(left + pw - 23, x)
      );
      ctx.fillText(label, labelX, top + ph + 20);
      ctx.textBaseline = 'middle';
    }
  }
  // ----- Right-side current axis -----
  for (let a = 0; a <= aMax + 0.001; a += 0.5) {
    const y = top + ph - (a / aMax) * ph;
    ctx.fillStyle = '#222';
    ctx.textAlign = 'left';
    ctx.fillText(a.toFixed(1), left + pw + 6, y);
    ctx.beginPath();
    ctx.strokeStyle = '#666';
    ctx.lineWidth = 1;
    ctx.moveTo(left + pw, y);
    ctx.lineTo(left + pw + 4, y);
    ctx.stroke();
  }
  // Plot boundary.
  ctx.strokeStyle = '#777';
  ctx.lineWidth = 1;
  ctx.strokeRect(left, top, pw, ph);
  if (!plotPoints.length) {
    ctx.fillStyle = '#555';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.font = '13px sans-serif';
    ctx.fillText(
      'Start a test or press Monitor to plot live data',
      left + pw / 2,
      top + ph / 2
    );
    return;
  }
  // Draw traces.
  const drawTrace = (key, max, color) => {
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.8;
    plotPoints.forEach((p, i) => {
      const x = left + ((p.t - t0) / duration) * pw;
      const y = top + ph - Math.max(0, Math.min(1, p[key] / max)) * ph;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  };
  drawTrace('v', vMax, '#1769d2');
  drawTrace('a', aMax, '#d52b2b');
}


//////////////////////////// storage

function formatStorageBytes(bytes) {
  if (!Number.isFinite(bytes)) return '--';
  const mib = bytes / (1024 * 1024);
  return mib.toFixed(2) + ' MiB';
}


function updateStorageDisplay(data) {
  const storage = data?.storage;
  if (!storage) return;
  const psram = storage.psram || {};
  const fs = storage.littlefs || {};
  // Header summary
  const psramPercent = psram.available ? Number(psram.usage_percent).toFixed(1) : '--';
  const fsPercent = fs.available ? Number(fs.usage_percent).toFixed(1) : '--';
  $('storageSummary').textContent = `Storage · PSRAM: ${psramPercent}% Used · FS: ${fsPercent}% Used`;
  // PSRAM details
  $('psramStatus').textContent = psram.available ? 'Available' : 'Not available';
  const psramUsed = Math.max(0, (psram.total || 0) - (psram.free || 0));
  $('psramTotal').textContent = formatStorageBytes(psram.total);
  $('psramUsed').textContent = formatStorageBytes(psramUsed);
  $('psramFree').textContent = formatStorageBytes(psram.free);
  $('psramUsage').textContent = psram.available ? `${psramPercent}%` : '--';
  $('psramBar').style.width = psram.available ? `${Math.min(100, Number(psramPercent))}%` : '0%';
  // LittleFS details
  $('fsStatus').textContent = fs.mounted ? 'Mounted' : 'Not mounted';
  $('fsTotal').textContent = formatStorageBytes(fs.total);
  $('fsUsed').textContent = formatStorageBytes(fs.used);
  $('fsFree').textContent = formatStorageBytes(fs.free);
  $('fsUsage').textContent = fs.available ? `${fsPercent}%` : '--';
  $('fsBar').style.width = fs.available ? `${Math.min(100, Number(fsPercent))}%` : '0%';
  // The backend independently refuses formatting during a test or cycle.
  // Mirror that state here to avoid offering an operation that cannot run.
  const cycleStatus = String(lastData?.cycle_status || 'idle').toLowerCase();
  const busy = !!lastData?.running || cycleStatus === 'running' || cycleStatus === 'paused';
  const formatButton = $('formatFsBtn');
  formatButton.disabled = !fs.mounted || busy || formattingFilesystem;
  if (formattingFilesystem) {
    $('formatFsStatus').textContent = 'Formatting filesystem…';
  } else if (!fs.mounted) {
    $('formatFsStatus').textContent = 'Formatting unavailable: LittleFS is not mounted.';
  } else if (busy) {
    $('formatFsStatus').textContent = 'Pause or finish the active test/cycle before formatting.';
  } else if (!formatStatusMessage) {
    $('formatFsStatus').textContent = 'Ready. Formatting permanently deletes all saved test results.';
  } else {
    $('formatFsStatus').textContent = formatStatusMessage;
  }
}


let formattingFilesystem = false;
let formatStatusMessage = '';

async function formatFilesystem() {
  if (formattingFilesystem || $('formatFsBtn').disabled) return;
  const confirmed = confirm(
    'FORMAT THE LITTLEFS FILESYSTEM?\n\n' +
    'This permanently deletes ALL saved test results and graph images.\n\n' +
    'This action cannot be undone.'
  );
  if (!confirmed) return;
  formattingFilesystem = true;
  formatStatusMessage = '';
  updateStorageDisplay(lastData);
  try {
    const response = await fetch('/api/format', {
      method: 'POST',
      cache: 'no-store'
    });
    let result;
    try {
      result = await response.json();
    } catch (_) {
      throw new Error(`Invalid response from controller (HTTP ${response.status})`);
    }
    if (!response.ok || !result.ok) {
      throw new Error(result.error || `Filesystem format failed (HTTP ${response.status})`);
    }
    formatStatusMessage = result.message || 'Filesystem formatted successfully.';
    selectedMainResultId = null;
    selectedCycleResultId = null;
    liveGraphRecordId = null;
    liveGraphReady = false;
    // Formatting removes every persisted record. Refresh both result views.
    await Promise.all([loadMainResults(), loadCycleResults()]);
    await refresh();
  } catch (error) {
    formatStatusMessage = `Format failed: ${error.message}`;
    console.error('Filesystem format failed:', error);
    alert(formatStatusMessage);
  } finally {
    formattingFilesystem = false;
    updateStorageDisplay(lastData);
  }
}


function openStorageDialog() {
  $('storageDialog').showModal();
}


function closeStorageDialog() {
  $('storageDialog').close();
}


window.addEventListener('resize', drawGraph);
updateControls();
renderCycleRows();
renderCycleResults();
loadCycleStep();
drawGraph();
refresh();
setInterval(refresh, 1000);



