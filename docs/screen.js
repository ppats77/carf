const descriptions = {
  drive: "Simulated driving screen with 1,852 RPM, 48 km/h and 86 °C coolant.",
  storage: "Storage screen prompting for a FAT32 microSD card, with a 12-second retry countdown.",
  paused: "Capture paused on the 2007 BMW 335i profile. The middle touch control reads Resume."
};
const controls = document.querySelector(".switches");
const screenImage = document.querySelector("#screen");
for (const button of controls.querySelectorAll("button")) {
  button.addEventListener("click", () => {
    const state = button.dataset.screen;
    screenImage.src = "assets/" + state + ".png";
    screenImage.alt = descriptions[state];
    for (const choice of controls.querySelectorAll("button")) {
      choice.setAttribute("aria-pressed", String(choice === button));
    }
  });
}
controls.hidden = false;
