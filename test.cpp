#include "transmission_webui.hpp"
#include "utorrent_webui.hpp"
#include "deluge.hpp"
#include "file_downloader.hpp"
#include "auto_load.hpp"
#include "save_settings.hpp"

#include "libtorrent/session.hpp"

#include <signal.h>

using namespace libtorrent;

int main(int argc, char *const argv[])
{
	session ses(fingerprint("LT", 0, 1, 0, 0)
		, std::make_pair(6881, 6882));

	save_settings sett(ses, "settings.dat");

	error_code ec;
	sett.load(ec);

	auto_load al(ses, &sett);

	transmission_webui tr_handler(ses, &sett);
	utorrent_webui ut_handler(ses, &sett, &al);
	file_downloader file_handler(ses);

	webui_base webport;
	webport.add_handler(&ut_handler);
	webport.add_handler(&tr_handler);
	webport.add_handler(&file_handler);
	webport.start(8080);

	deluge dlg(ses, "server.pem");
	dlg.start(58846);

	// don't terminate on these, since we want
	// to shut down gracefully
	signal(SIGTERM, SIG_IGN);
	signal(SIGINT, SIG_IGN);

	sigset_t sigset;
	sigfillset(&sigset);
	sigdelset(&sigset, SIGTERM);
	sigdelset(&sigset, SIGINT);
	// now, just wait to be shutdown
	sigsuspend(&sigset);

	dlg.stop();
	webport.stop();
	sett.save(ec);
}

