// The main window (main_window.h): Options → Discord status, and the numbers handed to the presence thread
// (core/discord_presence.h). Off by default: turned on, it tells Discord (and so your Discord friends) what
// you are working on.
#include <system_error>

#include "core/discord_presence.h"
#include "core/util.h"
#include "ui/main_window.h"
#include "ui/toolkit/dialogs.h"

void MainWindow::addDiscordOptions(ui::Menu* menu) {
    ui::Menu* discord = menu->addMenu("&Discord status");
    discord->addCheckable("&Show in Discord", prefs.discordPresence, [this](bool on) {
            prefs.discordPresence = on;
            prefs.save();
            configureDiscordPresence();
        })->tooltip = "Discord (running on this computer) shows your friends that you use Exbit: how many audio "
                      "and MIDI lanes the project has, the audio CPU load and Exbit's memory";
    discord->addAction("&Application ID…", [this] { chooseDiscordApplicationId(); })->tooltip =
        "Exbit's own Discord application unless you enter the ID of another (made in Discord's developer "
        "portal): Discord shows its name above the text";
    discord->addSeparator();
    discordStateAction = discord->addAction("");
    discordStateAction->enabled = false;
    discordStateAction->tooltip = "How it goes: Discord not running, or the ID refused, is tried again by itself";
    discord->aboutToShow = [this](ui::Menu*) { showDiscordState(); };
    showDiscordState();
}

void MainWindow::startDiscordPresence() {
    try {
        discordPresence = std::make_unique<discord_presence::Presence>();
    } catch (const std::system_error& e) {  // (no thread to be had: the program goes on without it)
        util::debugf("discord: the presence thread couldn't start: %s\n", e.what());
        discordPresence.reset();
    }
    configureDiscordPresence();
}

void MainWindow::configureDiscordPresence() {
    if (discordPresence)
        discordPresence->configure(prefs.discordPresence, discord_presence::effectiveApplicationId(prefs.discordApplicationId));
    showDiscordState();
}

void MainWindow::chooseDiscordApplicationId() {
    std::string id = prefs.discordApplicationId;
    if (!ui::getText(this, "Discord application ID",
                     "Empty: Exbit's own application. Another: make one at discord.com/developers/applications "
                     "(New Application), then copy its Application ID here.",
                     id))
        return;
    prefs.discordApplicationId = util::trim(id);
    prefs.save();
    configureDiscordPresence();
}

void MainWindow::showDiscordState() {
    if (!discordStateAction) return;
    discordStateAction->text =
        discordPresence ? "Status: " + discordPresence->stateText() : std::string("Status: unavailable (no thread for it)");
}

void MainWindow::feedDiscordPresence(double cpuLoad) {
    if (!discordPresence || !prefs.discordPresence) return;
    int audioLanes = 0, midiLanes = 0;
    for (auto& track : *project.tracks) {
        if (track->kind == Kind::Audio) audioLanes++;
        if (track->kind == Kind::Midi) midiLanes++;
    }
    discordPresence->setUsage(audioLanes, midiLanes, cpuLoad);
}
