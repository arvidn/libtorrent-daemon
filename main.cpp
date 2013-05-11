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

struct error_logger : libtorrent::alert_observer
{
	error_logger(alert_handler* alerts, std::string const& log_file, bool redirect_stderr)
		: m_file(NULL)
		, m_alerts(alerts)
	{
		if (!log_file.empty())
		{
			m_file = fopen(log_file.c_str(), "a");
			if (m_file == NULL)
			{
				fprintf(stderr, "failed to open error log \"%s\": (%d) %s\n"
					, log_file.c_str(), errno, strerror(errno));
			}
			else if (redirect_stderr)
			{
				dup2(fileno(m_file), STDOUT_FILENO);
				dup2(fileno(m_file), STDERR_FILENO);
			}
			m_alerts->subscribe(this, 0
				, peer_disconnected_alert::alert_type
				, peer_error_alert::alert_type
				, save_resume_data_failed_alert::alert_type
				, torrent_delete_failed_alert::alert_type
				, storage_moved_failed_alert::alert_type
				, file_rename_failed_alert::alert_type
				, 0);
		}
	}

	~error_logger()
	{
		m_alerts->unsubscribe(this);
		if (m_file) fclose(m_file);
	}

	void handle_alert(alert const* a)
	{
		if (m_file == NULL) return;
		time_t now = time(NULL);
		char timestamp[256];
		strncpy(timestamp, ctime(&now), sizeof(timestamp));
		for (int i = 0; i < sizeof(timestamp); ++i)
		{
			if (timestamp[i] != '\n' && timestamp[i] != '\r') continue;
			timestamp[i] = '\0';
			break;
		}

		switch (a->type())
		{
			case peer_error_alert::alert_type:
			{
				peer_error_alert const* pe = alert_cast<peer_error_alert>(a);
#ifdef TORRENT_USE_OPENSSL
				// unknown protocol
				if (pe->error != error_code(336027900, boost::asio::error::get_ssl_category()))
#endif
				{
					fprintf(m_file, "%s\terror [%s] (%s:%d) %s\n", timestamp
						, print_endpoint(pe->ip).c_str(), pe->error.category().name()
						, pe->error.value(), pe->error.message().c_str());
				}
				break;
			}
			case peer_disconnected_alert::alert_type:
			{
				peer_disconnected_alert const* pd = alert_cast<peer_disconnected_alert>(a);
				if (pd
					&& pd->error != boost::system::errc::connection_reset
					&& pd->error != boost::system::errc::connection_aborted
					&& pd->error != boost::system::errc::connection_refused
					&& pd->error != boost::system::errc::timed_out
					&& pd->error != boost::asio::error::eof
					&& pd->error != boost::asio::error::host_unreachable
					&& pd->error != boost::asio::error::network_unreachable
					&& pd->error != boost::asio::error::broken_pipe
#ifdef TORRENT_USE_OPENSSL
					// unknown protocol
					&& pd->error != error_code(336027900, boost::asio::error::get_ssl_category())
#endif
					&& pd->error != error_code(libtorrent::errors::self_connection)
					&& pd->error != error_code(libtorrent::errors::torrent_removed)
					&& pd->error != error_code(libtorrent::errors::torrent_aborted)
					&& pd->error != error_code(libtorrent::errors::stopping_torrent)
					&& pd->error != error_code(libtorrent::errors::session_closing)
					&& pd->error != error_code(libtorrent::errors::duplicate_peer_id)
					&& pd->error != error_code(libtorrent::errors::timed_out)
					&& pd->error != error_code(libtorrent::errors::timed_out_no_handshake)
					&& pd->error != error_code(libtorrent::errors::upload_upload_connection))
					fprintf(m_file, "%s\tdisconnect [%s][%s] (%s:%d) %s\n", timestamp
						, print_endpoint(pd->ip).c_str(), operation_name(pd->operation)
						, pd->error.category().name(), pd->error.value(), pd->error.message().c_str());
				break;
			}
			case save_resume_data_failed_alert::alert_type:
			{
				save_resume_data_failed_alert const* rs= alert_cast<save_resume_data_failed_alert>(a);
				if (rs)
					fprintf(m_file, "%s\tsave-resume-failed (%s:%d) %s\n", timestamp
						, rs->error.category().name(), rs->error.value()
						, rs->message().c_str());
			}
			case torrent_delete_failed_alert::alert_type:
			{
				torrent_delete_failed_alert const* td = alert_cast<torrent_delete_failed_alert>(a);
				if (td)
					fprintf(m_file, "%s\tstorage-delete-failed (%s:%d) %s\n", timestamp
						, td->error.category().name(), td->error.value()
						, td->message().c_str());
			}
			case storage_moved_failed_alert::alert_type:
			{
				storage_moved_failed_alert const* sm = alert_cast<storage_moved_failed_alert>(a);
				if (sm)
					fprintf(m_file, "%s\tstorage-move-failed (%s:%d) %s\n", timestamp
						, sm->error.category().name(), sm->error.value()
						, sm->message().c_str());
			}
			case file_rename_failed_alert::alert_type:
			{
				file_rename_failed_alert const* rn = alert_cast<file_rename_failed_alert>(a);
				if (rn)
					fprintf(m_file, "%s\tfile-rename-failed (%s:%d) %s\n", timestamp
						, rn->error.category().name(), rn->error.value()
						, rn->message().c_str());
			}
		}
	}

private:
	FILE* m_file;
	alert_handler* m_alerts;
};

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

	alert_handler alerts;

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

	transmission_webui tr_handler(ses, &sett, &authorizer);
	utorrent_webui ut_handler(ses, &sett, &al, &hist, &authorizer);
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

