/*
 * Copyright (C) Roland Jax 2014 <roland.jax@liwest.at>
 *
 * This file is part of ebus-daemon.
 *
 * ebus-daemon is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ebus-daemon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ebus-daemon. If not, see http://www.gnu.org/licenses/.
 */
#include "libebus.h"
#include "logger.h"
#include "daemon.h"
#include "appl.h"
#include "network.h"
#include "ebusloop.h"
#include "cycdata.h"
#include "baseloop.h"
#include <iostream>
#include <memory>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

using namespace libebus;

Appl& A = Appl::Instance();
Daemon& D = Daemon::Instance();
LogInstance& L = LogInstance::Instance();

Network* network;
Commands* commands;
EBusLoop* ebusloop;
CYCData* cycdata;

void define_args()
{
	A.addItem("p_address", Appl::Param("FF"), "a", "address",
	"\tebus device address (FF)",
	Appl::type_string, Appl::opt_mandatory);
	
	A.addItem("p_device", Appl::Param("/dev/ttyUSB0"), "d", "device",
	"\tebus device (serial or network) (/dev/ttyUSB0)",
	Appl::type_string, Appl::opt_mandatory);

	A.addItem("p_nodevicecheck", Appl::Param(false), "n", "nodevicecheck",
	"disable valid ebus device test\n", Appl::type_bool, Appl::opt_none);

	A.addItem("p_ebusconfdir", Appl::Param("contrib/csv/vaillant"), "e", "ebusconfdir",
	"directory for ebus configuration (contrib/csv/vaillant)\n",
	Appl::type_string, Appl::opt_mandatory);

	A.addItem("p_foreground", Appl::Param(false), "f", "foreground",
	"run in foreground\n", Appl::type_bool, Appl::opt_none);

	A.addItem("p_port", Appl::Param(8888), "p", "port",
	"\tlisten port (8888)", Appl::type_int, Appl::opt_mandatory);

	A.addItem("p_localhost", Appl::Param(false), "", "localhost",
	"listen localhost only\n", Appl::type_bool, Appl::opt_none);

	A.addItem("p_area", Appl::Param(all), "", "logarea",
	"\tlogging area (bas=1, net=2, bus=4, cyc=8, all=15)",
	Appl::type_int, Appl::opt_mandatory);

	A.addItem("p_level", Appl::Param(trace), "", "loglevel",
	"\tlogging level (error=1, event=2, trace=3, debug=4)\n",
	Appl::type_int, Appl::opt_mandatory);

	A.addItem("p_dump", Appl::Param(false), "D", "dump",
	"\tenable dump", Appl::type_bool, Appl::opt_none);

	A.addItem("p_dumpfile", Appl::Param("/tmp/dump_ebusd.bin"), "", "dumpfile",
	"\tdump file name (/tmp/dump_ebusd.bin)", Appl::type_string, Appl::opt_mandatory);

	A.addItem("p_dumpsize", Appl::Param(100), "", "dumpsize",
	"\tmax size for dump file in kB (100)\n", Appl::type_long, Appl::opt_mandatory);
	
	A.addItem("p_help", Appl::Param(false), "h", "help",
	"\tprint this message", Appl::type_bool, Appl::opt_none);
}

void shutdown()
{
	// free Network
	if (network != NULL)
		delete network;

	// free CYCData
	if (cycdata != NULL) {
		cycdata->stop();
		delete cycdata;
	}

	// free EBusLoop
	if (ebusloop != NULL) {
		ebusloop->stop();
		ebusloop->join();
		delete ebusloop;
	}
	
	// free Commands DB
	if (commands != NULL)
		delete commands;

	// reset all signal handlers to default
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);

	// delete Daemon pid file
	if (D.status() == true)
		D.stop();

	// stop Logger
	L.log(bas, event, "ebusd stopped");
	L.stop();
	L.join();

	exit(EXIT_SUCCESS);
}

void signal_handler(int sig)
{
	switch (sig) {
	case SIGHUP:
		L.log(bas, event, "SIGHUP received");
		break;
	case SIGINT:
		L.log(bas, event, "SIGINT received");
		shutdown();
		break;
	case SIGTERM:
		L.log(bas, event, "SIGTERM received");
		shutdown();
		break;
	default:
		L.log(bas, event, "undefined signal %s", strsignal(sig));
		break;
	}
}

int main(int argc, char* argv[])
{
	// define Arguments and Application variables
	define_args();

	// parse Arguments
	if (A.parse(argc, argv) == false) {
		A.printArgs();
		exit(EXIT_FAILURE);
	}
	
	// print Help
	if (A.getParam<bool>("p_help") == true) {
		A.printArgs();
		exit(EXIT_SUCCESS);
	}
	
	// make me Daemon
	if (A.getParam<bool>("p_foreground") == true) {
		L += new LogConsole(A.getParam<int>("p_area"),
			static_cast<const Level>(A.getParam<int>("p_level")), "logConsole");
	} else {
		D.run("/var/run/ebusd.pid");
		L += new LogFile(A.getParam<int>("p_area"),
			static_cast<const Level>(A.getParam<int>("p_level")), "logFile", "/var/log/ebusd.log");
	}

	// trap Signals that we expect to receive
	signal(SIGHUP, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	// start Logger
	L.start("logInstance");
	L.log(bas, event, "ebusd started");

	// print Daemon status
	if (D.status() == true)
		L.log(bas, event, "change to daemon");

	// create Commands DB
	commands = ConfigCommands(A.getParam<const char*>("p_ebusconfdir"), CSV).getCommands();
	L.log(bas, event, "commands DB with %d entries created", commands->size());

	// create EBusLoop
	ebusloop = new EBusLoop();
	ebusloop->start("ebusloop");

	// create CYCData
	cycdata = new CYCData(ebusloop, commands);
	cycdata->start("cycdata");

	// create Network
	network = new Network(A.getParam<bool>("p_localhost"));

	// create BaseLoop
	BaseLoop baseloop(ebusloop, cycdata, commands);

	// start Network
	network->addQueue(baseloop.getQueue());
	network->start("netListener");

	// start Baseloop
	baseloop.start();

	shutdown();
}

