/*
 Copyright (C) 2017-2019 Fredrik Öhrström

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include"cmdline.h"
#include"config.h"
#include"meters.h"
#include"printer.h"
#include"serial.h"
#include"util.h"
#include"wmbus.h"

#include<string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/errno.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>

using namespace std;

void oneshotCheck(CommandLine *cmdline, SerialCommunicationManager *manager, Meter *meter, vector<unique_ptr<Meter>> &meters);
void startUsingCommandline(CommandLine *cmdline);
void startUsingConfigFiles(string root, bool is_daemon);
void startDaemon(string pid_file); // Will use config files.

int main(int argc, char **argv)
{
    auto cmdline = parseCommandLine(argc, argv);

    if (cmdline->need_help) {
        printf("wmbusmeters version: " WMBUSMETERS_VERSION "\n");
        printf("Usage: wmbusmeters [options] (auto | /dev/ttyUSBx) { [meter_name] [meter_type] [meter_id] [meter_key] }* \n\n");
        printf("Add more meter quadruplets to listen to more meters.\n");
        printf("Add --verbose for more detailed information on communication.\n");
        printf("    --robot or --robot=json for json output.\n");
        printf("    --robot=fields for semicolon separated fields.\n");
        printf("    --separator=X change field separator to X.\n");
        printf("    --meterfiles=dir to create status files below dir,\n"
               "        named dir/meter_name, containing the latest reading.\n");
        printf("    --meterfiles defaults dir to /tmp.\n");
        printf("    --shell=cmd invokes cmd with env variables containing the latest reading.\n");
        printf("    --shellenvs list the env variables available for the meter.\n");
        printf("    --oneshot wait for an update from each meter, then quit.\n\n");
        printf("    --exitafter=20h program exits after running for twenty hoursh\n"
               "        or 10m for ten minutes or 5s for five seconds.\n");
        printf("    --useconfig read from /etc/wmbusmeters.conf and /etc/wmbusmeters.d\n");
        printf("        check the man page for how to write the config files.\n");
        printf("    --reload signals a running wmbusmeters daemon to reload the configuration,\n");
        printf("        when you have modified config files and/or usb dongles.\n\n");

        printf("Specifying auto as the device will automatically look for usb\n");
        printf("wmbus dongles on /dev/im871a and /dev/amb8465\n\n");
        printf("The meter types: multical21,flowiq3100,supercom587,iperl (water meters) are supported.\n"
               "The meter types: multical302 (heat) and omnipower (electricity) qcaloric (heat cost)\n"
               "are work in progress.\n\n");
    }
    else
    if (cmdline->daemon) {
        startDaemon(cmdline->pid_file);
        exit(0);
    }
    else
    if (cmdline->useconfig) {
        // This is primarily used for testing.
        const char *r = getenv("WMBUSMETERS_CONFIG_ROOT");
        string root = "";
        if (r != NULL) {
            root = r;
        }
        startUsingConfigFiles(root, false);
        exit(0);
    }
    else {
        // We want the data visible in the log file asap!
        setbuf(stdout, NULL);
        startUsingCommandline(cmdline.get());
    }
}

void startUsingCommandline(CommandLine *cmdline)
{
    warningSilenced(cmdline->silence);
    verboseEnabled(cmdline->verbose);
    logTelegramsEnabled(cmdline->logtelegrams);
    debugEnabled(cmdline->debug);

    if (cmdline->exitafter != 0) {
        verbose("(cmdline) wmbusmeters will exit after %d seconds\n", cmdline->exitafter);
    }

    if (cmdline->meterfiles) {
        verbose("(cmdline) store meter files in: \"%s\"\n", cmdline->meterfiles_dir.c_str());
    }
    verbose("(cmdline) using usb device: %s\n", cmdline->usb_device.c_str());
    verbose("(cmdline) number of meters: %d\n", cmdline->meters.size());

    auto manager = createSerialCommunicationManager(cmdline->exitafter);

    onExit(call(manager.get(),stop));

    unique_ptr<WMBus> wmbus;

    auto type_and_device = detectMBusDevice(cmdline->usb_device, manager.get());

    switch (type_and_device.first) {
    case DEVICE_IM871A:
        verbose("(im871a) detected on %s\n", type_and_device.second.c_str());
        wmbus = openIM871A(type_and_device.second, manager.get());
        break;
    case DEVICE_AMB8465:
        verbose("(amb8465) detected on %s\n", type_and_device.second.c_str());
        wmbus = openAMB8465(type_and_device.second, manager.get());
        break;
    case DEVICE_SIMULATOR:
        verbose("(simulator) found %s\n", type_and_device.second.c_str());
        wmbus = openSimulator(type_and_device.second, manager.get());
        break;
    case DEVICE_UNKNOWN:
        warning("No wmbus device found! Exiting!\n");
        if (cmdline->daemon) {
            // If starting as a daemon, wait a bit so that systemd have time to catch up.
            sleep(1);
        }
        exit(1);
        break;
    }

    if (!cmdline->link_mode_set) {
        // The link mode is not explicitly set. Examine the meters to see which
        // link mode to use.
        for (auto &m : cmdline->meters) {
            if (!cmdline->link_mode_set) {
                cmdline->link_mode = toMeterLinkMode(m.type);
                cmdline->link_mode_set = true;
            } else {
                if (cmdline->link_mode != toMeterLinkMode(m.type)) {
                    error("A different link mode has been set already.\n");
                }
            }
        }
    }
    if (!cmdline->link_mode_set) {
        error("If you specify no meters, you have to specify the link mode: --c1 or --t1\n");
    }
    wmbus->setLinkMode(cmdline->link_mode);
    string using_link_mode = linkModeName(wmbus->getLinkMode());

    verbose("(cmdline) using link mode: %s\n", using_link_mode.c_str());

    auto output = unique_ptr<Printer>(new Printer(cmdline->json, cmdline->fields,
                                                   cmdline->separator, cmdline->meterfiles, cmdline->meterfiles_dir,
                                                   cmdline->shells));
    vector<unique_ptr<Meter>> meters;

    if (cmdline->meters.size() > 0) {
        for (auto &m : cmdline->meters) {
            const char *keymsg = (m.key[0] == 0) ? "not-encrypted" : "encrypted";
            switch (toMeterType(m.type)) {
            case MULTICAL21_METER:
                meters.push_back(createMultical21(wmbus.get(), m.name, m.id, m.key));
                verbose("(multical21) configured \"%s\" \"multical21\" \"%s\" %s\n", m.name.c_str(), m.id.c_str(), keymsg);
                break;
            case FLOWIQ3100_METER:
                meters.push_back(createMultical21(wmbus.get(), m.name, m.id, m.key));
                verbose("(flowiq3100) configured \"%s\" \"flowiq3100\" \"%s\" %s\n", m.name.c_str(), m.id.c_str(), keymsg);
                break;
            case MULTICAL302_METER:
                meters.push_back(createMultical302(wmbus.get(), m.name, m.id, m.key));
                verbose("(multical302) configured \"%s\" \"multical302\" \"%s\" %s\n", m.name.c_str(), m.id.c_str(), keymsg);
                break;
            case OMNIPOWER_METER:
                meters.push_back(createOmnipower(wmbus.get(), m.name, m.id, m.key));
                verbose("(omnipower) configured \"%s\" \"omnipower\" \"%s\" %s\n", m.name.c_str(), m.id.c_str(), keymsg);
                break;
            case SUPERCOM587_METER:
                meters.push_back(createSupercom587(wmbus.get(), m.name, m.id, m.key));
                verbose("(supercom587) configured \"%s\" \"supercom587\" \"%s\" %s\n", m.name.c_str(), m.id.c_str(), keymsg);
                break;
            case IPERL_METER:
                meters.push_back(createIperl(wmbus.get(), m.name, m.id, m.key));
                verbose("(iperl) configured \"%s\" \"iperl\" \"%s\" %s\n", m.name.c_str(), m.id.c_str(), keymsg);
                break;
            case QCALORIC_METER:
                meters.push_back(createQCaloric(wmbus.get(), m.name, m.id, m.key));
                verbose("(qcaloric) configured \"%s\" \"qcaloric\" \"%s\" %s\n", m.name.c_str(), m.id.c_str(), keymsg);
                break;
            case UNKNOWN_METER:
                error("No such meter type \"%s\"\n", m.type.c_str());
                break;
            }
            if (cmdline->list_shell_envs) {
                string ignore1, ignore2, ignore3;
                vector<string> envs;
                meters.back()->printMeter(&ignore1,
                                          &ignore2, cmdline->separator,
                                          &ignore3,
                                          &envs);
                printf("Environment variables provided to shell for meter %s:\n", m.type.c_str());
                for (auto &e : envs) {
                    int p = e.find('=');
                    string key = e.substr(0,p);
                    printf("%s\n", key.c_str());
                }
                exit(0);
            }
            meters.back()->onUpdate(calll(output.get(),print,Meter*));
            meters.back()->onUpdate([&](Meter*meter) { oneshotCheck(cmdline, manager.get(), meter, meters); });
        }
    } else {
        printf("No meters configured. Printing id:s of all telegrams heard!\n\n");

        wmbus->onTelegram([](Telegram *t){t->print();});
    }

    if (type_and_device.first == DEVICE_SIMULATOR) {
        wmbus->simulate();
    }

    notice("wmbusmeters waiting for telegrams\n");
    manager->waitForStop();
}

void oneshotCheck(CommandLine *cmdline, SerialCommunicationManager *manager, Meter *meter, vector<unique_ptr<Meter>> &meters)
{
    if (!cmdline->oneshot) return;

    for (auto &m : meters) {
        if (m->numUpdates() == 0) return;
    }
    // All meters have received at least one update! Stop!
    manager->stop();
}

void writePid(string pid_file, int pid)
{
    FILE *pidf = fopen(pid_file.c_str(), "w");
    if (!pidf) {
        error("Could not open pid file \"%s\" for writing!\n", pid_file.c_str());
    }
    if (pid > 0) {
        int n = fprintf(pidf, "%d\n", pid);
        notice("writing pid %s\n", pid_file.c_str());
        if (!n) {
            error("Could not write pid (%d) to file \"%s\"!\n", pid, pid_file.c_str());
        }
    }
    fclose(pidf);
    return;
}

void startDaemon(string pid_file)
{
    setlogmask(LOG_UPTO (LOG_INFO));
    openlog("wmbusmetersd", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);

    enableSyslog();

    notice("wmbusmeters starting...\n");

    // Pre check that the pid file can be writte to.
    // Exit before fork, if it fails.
    writePid(pid_file, 0);

    pid_t pid = fork();
    if (pid < 0)
    {
        error("Could not fork.\n");
    }
    if (pid > 0)
    {
        // Success! The parent stores the pid and exits.
        writePid(pid_file, pid);
        return;
    }

    // Change the file mode mask
    umask(0);

    // Create a new SID for the daemon
    pid_t sid = setsid();
    if (sid < 0) {
        // log
        exit(-1);
    }

    if ((chdir("/")) < 0) {
        error("Could not change to root as current working directory.");
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    startUsingConfigFiles("", true);
}

void startUsingConfigFiles(string root, bool is_daemon)
{
    unique_ptr<CommandLine> cmdline = loadConfiguration(root);
    cmdline->daemon = is_daemon;

    startUsingCommandline(cmdline.get());
}
