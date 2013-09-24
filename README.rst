libtorrent-daemon
-----------------

libtorrent-daemon is a bittorrent client specifically made for servers.

It includes support for using the uTorrent web UI or the transmission web UI.

Prominent features include:

* serving web UI over SSL
* user accounts with varying level of access to settings (controlled via separate password file)
* auto-load directory
* HTTP POST of .torrent files for adding
* downloading files from torrents via an HTTP interface (i.e. uTorrent /proxy requests)
* deamonization
* error and debug log file
* pid file
* auto-load directory
* periodic saving of resume files
* simple plain text configuration file (which overrides any settings)
* save and resore of settings in settings.dat

building
--------

libtorrent-daemon depends on libtorrent_ and libtorrent-webui_.

The Jamfile in the root expects those directories to be checked out
next to the libtorrentp-daemon directory.

libtorrent in turn depend boost, and on openssl( in case it's built with encryption=openssl).

To build, you need boost-build and openssl installed.

.. _libtorrent: http://libtorrent.org
.. _libtorrent-webui: http://www.github.com/arvidn/libtorrent-webui

To build, invoke the command::

	bjam

If you have a directory with boost instead of having it installed, make sure the
environment variable ``BOOST_ROOT`` is set to that directory and invoke::

	bjam boost=source

This should produce two executables in the root directory of libtorrent-deamon:

* libtorrent-deamon
* add_user

deploying
---------

In order to deploy using the uTorrent webUI. Unzip the webui.zip as a ``gui``
directory from the current working directory of the daemon.

To deploy with transmission webui, place the javascript and html files in a
``web`` directory in the current working directory of the daemon.

To set up users, use the ``add_user`` command line tool like this:

.. parsed-literal:

	add_user *user-name* *user-group-id*

The group IDs are:

0. admin user, full access
1. read-only user
2. limited user (can add and remove torrents, but not change configurations)

command line arguments
----------------------

Most of the command line arguments are straight forward and explained by
running ``libtorrent-daemon --help``.

The configuration file format is::

	key: value

Where key is the name of a `libtorrent setting`_.

In order to make libtorrent-daemon accept webui request over SSL, you need to
provide a ``.pem``-file to the ``-s`` option.

The user configuration file supports three levels of users. full access,
limited access and read only. These categories of users are groups 0, 1 and 2
respectively. In order to generate a ``users.conf`` file, use the ``add_user``
tool from the ``libtorrent-webui`` repo.

User configuration files are only loaded on startup. If you add or remove a
user, you need to restart ``libtorrent-daemon`` for it to take effect.

.. _`libtorrent setting`: http://www.rasterbar.com/products/libtorrent/manual.html#session-customization


