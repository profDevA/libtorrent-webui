#include "transmission_webui.hpp"
#include "utorrent_webui.hpp"
#include "deluge.hpp"
#include "file_downloader.hpp"
#include "torrent_post.hpp"
#include "libtorrent/session.hpp"

using namespace libtorrent;

int main(int argc, char *const argv[])
{
	session ses(fingerprint("LT", 0, 1, 0, 0)
		, std::make_pair(6881, 6882));

	transmission_webui tr_handler(ses);
	utorrent_webui ut_handler(ses);
	file_downloader file_handler(ses);
	torrent_post tr_post(ses, "/upload"); // transmission-style
	// TODO: the filter needs to be more generic. uTorrent may add other arguments as well
	torrent_post ut_post(ses, "/gui/?action=add-file"); // utorrent-style

	webui_base webport;
	webport.add_handler(&ut_post);
	webport.add_handler(&tr_post);
	webport.add_handler(&ut_handler);
	webport.add_handler(&tr_handler);
	webport.add_handler(&file_handler);
	webport.start(8080);

	deluge dlg(ses, "server.pem");
	dlg.start(58846);

	while (getchar() != 'q');
//	while (true) sleep(1000);

	dlg.stop();
	webport.stop();
}

