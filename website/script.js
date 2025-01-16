// Global state variable to keep track of selected days
let globalDays = 0;
let globalHours = 0;
let globalMinutes = 0;
let globalTriggerLevel = 0;

  document.querySelectorAll(".day-button").forEach((button) => {
    button.addEventListener("click", () => {
      button.classList.toggle("selected");
    });
  });

  updateStats()
  .then(() => setDayButtons())
  .then(() => setTimePicker())
  .then(() => setTriggerInput());

  setInterval(() => {
    updateStats();
  }, 10000);

const dayMapping = {
  0: "Mon",
  1: "Tues",
  2: "Wed",
  3: "Thurs",
  4: "Fri",
  5: "Sat",
  6: "Sun",
};

function setDayButtons() {
  document.querySelectorAll(".day-button").forEach((button) => {
    if ((globalDays >> (parseInt(button.getAttribute('data-day'))) - 1) & 1) {
      button.classList.toggle("selected");
    }
  });
}

function setTimePicker() {
  const timeInput = document.getElementById("time-input");
  const formattedTime = `${String(globalHours).padStart(2, '0')}:${String(globalMinutes).padStart(2, '0')}`;
  timeInput.value = formattedTime;
}

function setTriggerInput() {
  const triggerInput = document.getElementById("trigger-input");
  triggerInput.value = String(globalTriggerLevel);
}

async function updateStats() {
  globalDays = 2;
  await fetch("/stats")
    .then((response) => response.json())
    .then((data) => {
      document.getElementById("water-level").innerText = data.level;
      document.getElementById("pump-state").innerText =
        data.pump_state.toUpperCase() == "TRUE" ? "ON" : "OFF";
      document.getElementById("current-system-time").innerText = data.current_system_time;
      globalTriggerLevel = data.trigger_level;
      globalDays = data.topup_dates;
      globalHours = data.topup_hour;
      globalMinutes = data.topup_minute;
    })
    .catch((err) => console.error("Error fetching water level:", err));
}

// Toggle the pump state"
function togglePump(state) {
  fetch(`/pump?state=${state}`, { method: "POST" })
    .then((response) => response.text())
    .then((data) => alert(data))
    .catch((err) => console.error("Error toggling pump:", err));
  updateStats();
}

// Set the trigger level
function setTriggerLevel() {
  const level = document.getElementById("trigger-input").value;
  fetch(`/set-trigger?level=${level}`, { method: "POST" })
    .then((response) => response.text())
    .then((data) => alert(data))
    .catch((err) => console.error("Error setting trigger level:", err));
  updateStats();
}

// Test water topup feature
function topUp() {
  fetch(`/topup`)
    .then((response) => response.text())
    .catch((err) => console.error("Error topping up water:", errr));
  updateStats();
}

function setSchedule() {
  const time = document.getElementById("time-input").value;
  const selectedDays = Array.from(
    document.querySelectorAll(".day-button.selected")
  ).map((button) => button.dataset.day);

  if (!time) {
    alert("Please set a valid time.");
    return;
  }
  const [hours, minutes] = time.split(":").map(Number);

  if (selectedDays.length === 0) {
    alert("Please select at least one day.");
    return;
  }
  let days = 0;
  selectedDays.forEach((value) => {
    const dayValue = parseInt(value);
    days |= 1 << (dayValue - 1);
  });

  let schedule = {
    time: {
      hours: hours,
      minutes,
      minutes,
    },
    days: days,
  };

  fetch("/topup/schedule", {
    method: "POST",
    body: JSON.stringify(schedule),
  })
    .then((response) => console.log(response.text()))
    .catch((err) => console.error("Error setting new schedule", err));
}
