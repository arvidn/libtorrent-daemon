#include "transmission_webui.hpp"
#include "utorrent_webui.hpp"
#include "deluge.hpp"
#include "file_downloader.hpp"
#include "auto_load.hpp"
#include "save_settings.hpp"
#include "save_resume.hpp"
#include "torrent_history.hpp"
#include "auth.hpp"
#include "load_config.hpp"
#include "http_whitelist.hpp"
#include "error_logger.hpp"
#include "rss_filter.hpp"

#include "libtorrent/session.hpp"
#include "libtorrent/socket_io.hpp" // for print_endpoint
#include "libtorrent/alert_handler.hpp"
#include "libtorrent/extensions/ut_metadata.hpp"
#include "libtorrent/extensions/ut_pex.hpp"

#include <signal.h>
#include <unistd.h> // for getpid()
#include <getopt.h> // for getopt_long
#include <stdlib.h> // for daemon()

bool quit = false;
bool force_quit = false;

void sighandler(int s)
{
	quit = true;
}

void sighandler_forcequit(int s)
{
	force_quit = true;
}

using namespace libtorrent;

// the limited user may not alter most confugurations, with the
// exception of download rate limits
struct limited_user : permissions_interface
{
	limited_user() {}
	virtual bool allow_start() const { return true; }
	virtual bool allow_stop() const { return true; }
	virtual bool allow_recheck() const { return true; }
	virtual bool allow_set_file_prio() const { return true; }
	virtual bool allow_list() const { return true; }
	virtual bool allow_add() const { return true; }
	virtual bool allow_remove() const { return true; }
	virtual bool allow_remove_data() const { return true; }
	virtual bool allow_queue_change() const { return true; }
	// the name is the constant used in settings_pack
	// or -1 for settings that don't fit a libtorrent setting
	bool allow_settings(int name) const
	{
		if (name < 0) return false;

		static int allowed_settings[] = {
			settings_pack::upload_rate_limit
			, settings_pack::download_rate_limit
			, settings_pack::unchoke_slots_limit
			, settings_pack::dht_upload_rate_limit
			, settings_pack::connections_limit
			, settings_pack::share_ratio_limit
			, settings_pack::seed_time_ratio_limit
			, settings_pack::seed_time_limit
			, settings_pack::active_downloads
			, settings_pack::active_seeds
			, settings_pack::auto_manage_prefer_seeds
			, settings_pack::cache_size
		};
		int num_allowed = sizeof(allowed_settings)/sizeof(allowed_settings[0]);
		if (std::find(allowed_settings, allowed_settings + num_allowed, name) == allowed_settings + num_allowed)
			return false;

		return true;
	}
	virtual bool allow_get_settings(int name) const { return allow_settings(name); }
	virtual bool allow_set_settings(int name) const { return allow_settings(name); }
	virtual bool allow_get_data() const { return true; }
	virtual bool allow_session_status() const { return true; }
};

static const limited_user limited_perms;

struct option cmd_line_options[] =
{
	{"config",            required_argument,   NULL, 'c'},
	{"pid",               required_argument,   NULL, 'p'},
	{"daemonize",         no_argument,         NULL, 'd'},
	{"listen-port",       required_argument,   NULL, 'l'},
	{"web-port",          required_argument,   NULL, 'w'},
	{"bind-interface",    required_argument,   NULL, 'i'},
	{"server-cert",       required_argument,   NULL, 's'},
	{"help",              no_argument,         NULL, 'h'},
	{"save-dir",          required_argument,   NULL, 'S'},
	{"users",             required_argument,   NULL, 'u'},
	{"error-log",         required_argument,   NULL, 'e'},
	{"debug-log",         required_argument,   NULL, 1},
	{NULL,                0,                   NULL, 0}
};

void print_usage()
{
	fputs("libtorrent-daemon usage:\n\n"
		"-c, --config           <config filename>\n"
		"-S, --save-dir         <download directory>\n"
		"-p, --pid              <pid-filename>\n"
		"-d, --daemonize\n"
		"-e, --error-log        <error log filename>\n"
		"    --debug-log        <debug log filename>\n"
		"-l, --listen-port      <bittorrent listen port>\n"
		"-w, --web-port         <http listen port>\n"
		"-i, --bind-interface   <comma separated IPs of interfaces to bind peer sockets to>\n"
		"-s, --server-cert      <.pem filename>\n"
		"-u, --users            <users filename>\n"
		"-h, --help\n"
		"\n"
		"user groups in user configuration file are defined as:\n"
		"0: root user (full permissions)\n"
		"1: read-only user (can only inspec state, not add or change anything)\n"
		"2: limited user (full permissions except only limited configuration access)\n"
		, stderr);
}

int main(int argc, char *const argv[])
{
	// general configuration of network ranges/peer-classes
	// and storage
	std::string config_file;

	bool daemonize = false;
	int listen_port = 6881;
	int web_listen_port = 8080;
	std::string cert;
	std::string listen_interface = "0.0.0.0";
	std::string bind_interface;
	std::string pid_file;
	std::string save_path;
	std::string users_file = "users.conf";
	std::string error_log;
	std::string debug_log;

	int ch = 0;
	while ((ch = getopt_long(argc, argv, "c:p:dl:w:i:s:hS:u:e:", cmd_line_options, NULL)) != -1)
	{
		switch (ch)
		{
			case 'c': config_file = optarg; break;
			case 'd': daemonize = true; break;
			case 'l': listen_port = atoi(optarg); break;
			case 'i': bind_interface = optarg; listen_interface = optarg; break;
			case 'w': web_listen_port = atoi(optarg); break;
			case 's': cert = optarg; break;
			case 'p': pid_file = optarg; break;
			case 'S': save_path = optarg; break;
			case 'u': users_file = optarg; break;
			case 'e': error_log = optarg; break;
			case 1: debug_log = optarg; break;
			default:
				print_usage();
				return 1;
		}
	}
	argc -= optind;
	argv += optind;

	if (daemonize) daemon(1, 0);

	auth authorizer;
	error_code ec;
	authorizer.load_accounts(users_file, ec);
	if (ec)
	{
		fprintf(stderr, "failed to load user accounts file: \"%s\": %s\n", users_file.c_str(), ec.message().c_str());
		return 1;
	}
	authorizer.set_group(2, &limited_perms);

	if (!pid_file.empty())
	{
		FILE* f = fopen(pid_file.c_str(), "w+");
		if (f)
		{
			fprintf(f, "%d", getpid());
			fclose(f);
		}
		else
		{
			fprintf(stderr, "failed to open pid file \"%s\": %s\n"
				, pid_file.c_str(), strerror(errno));
		}
	}

	FILE* debug_file = NULL;
	if (!debug_log.empty())
	{
		debug_file = fopen(debug_log.c_str(), "w+");
		if (debug_file == NULL)
		{
			fprintf(stderr, "failed to debug log \"%s\": %s\n"
				, debug_log.c_str(), strerror(errno));
		}
	}

	fprintf(stderr, "binding to \"%s\"\n", listen_interface.c_str());

	session ses(fingerprint("ld", 0, 1, 0, 0)
		, std::make_pair(listen_port, listen_port+1)
		, listen_interface.c_str(), 0);
	ses.set_alert_mask(~0);
	settings_pack s;
	high_performance_seed(s);
	ses.apply_settings(s);

	alert_handler alerts(ses);

	// start DHT
	ses.add_dht_router(std::make_pair(
		std::string("router.bittorrent.com"), 6881));
	ses.add_dht_router(std::make_pair(
		std::string("router.utorrent.com"), 6881));
	ses.add_dht_router(std::make_pair(
		std::string("router.bitcomet.com"), 6881));
	ses.start_dht();

	save_settings sett(ses, "settings.dat");
	sett.load(ec);

	if (!config_file.empty())
	{
		load_config(config_file, &ses, ec);
		if (ec) fprintf(stderr, "failed to load config: %s\n", ec.message().c_str());
	}

	if (!bind_interface.empty())
	{
		ses.use_interfaces(bind_interface.c_str());
	}

	torrent_history hist(&alerts);
	save_resume resume(ses, ".resume", &alerts);
	add_torrent_params p;
	if (!save_path.empty()) sett.set_str("save_path", save_path);
	p.save_path = sett.get_str("save_path", ".");
	resume.load(ec, p);

	error_logger el(&alerts, error_log, daemonize);

	auto_load al(ses, &sett);
	rss_filter_handler rss_filter(alerts, ses);

	transmission_webui tr_handler(ses, &sett, &authorizer);
	utorrent_webui ut_handler(ses, &sett, &al, &hist, &rss_filter, &authorizer);
	file_downloader file_handler(ses, &authorizer);
	http_whitelist whitelist;
	whitelist.add_allowed_prefix("gui"); // for utorrent webui
	whitelist.add_allowed_prefix("web"); // for transmission webui

	webui_base webport;
	webport.add_handler(&ut_handler);
	webport.add_handler(&tr_handler);
	webport.add_handler(&file_handler);
	webport.add_handler(&whitelist);

	webport.start(web_listen_port, cert.empty() ? NULL : cert.c_str(), 30);
	if (!webport.is_running())
	{
		fprintf(stderr, "failed to start web server\n");
		return 1;
	}

//	deluge dlg(ses, "server.pem");
//	dlg.start(58846);

	signal(SIGTERM, &sighandler);
	signal(SIGINT, &sighandler);
	signal(SIGPIPE, SIG_IGN);

	std::deque<alert*> alert_queue;
	bool shutting_down = false;
	while (!quit || !resume.ok_to_quit())
	{
		usleep(1000000);
		ses.pop_alerts(&alert_queue);
		if (debug_file)
		{
			for (std::deque<alert*>::iterator i = alert_queue.begin()
				, end(alert_queue.end()); i != end; ++i)
			{
				fprintf(debug_file, " %s\n", (*i)->message().c_str());
			}
		}
		alerts.dispatch_alerts(alert_queue);
		if (!shutting_down) ses.post_torrent_updates();
		if (quit && !shutting_down)
		{
			resume.save_all();
			shutting_down = true;
			signal(SIGTERM, &sighandler_forcequit);
			signal(SIGINT, &sighandler_forcequit);
		}
		if (force_quit) break;
	}

	if (debug_file) fclose(debug_file);

//	dlg.stop();
	webport.stop();
	sett.save(ec);
}

