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

To build, you need boost-build installed.

.. _libtorrent: http://libtorrent.org
.. _libtorrent-webui: http://www.github.com/arvidn/libtorrent-webui

deploying
---------

In order to deploy using the uTorrent webUI. Unzip the webui.zip as a ``gui``
directory from the current working directory of the daemon.

To deploy with transmission webui, place the javascript and html files in a
``web`` directory in the current working directory of the daemon.

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


