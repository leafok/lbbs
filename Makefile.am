ACLOCAL_AMFLAGS = -I m4

# Redirect pkg-config-detected absolute paths back into $(prefix) during distcheck's install phase.
DISTCHECK_CONFIGURE_FLAGS = --with-systemdsystemunitdir='$${sysconfdir}/systemd/system' --with-logrotatedir='$${sysconfdir}/logrotate.d'

EXTRA_DIST = README.md README.zh_CN.md INSTALL.md INSTALL.zh_CN.md ChangeLog.md ChangeLog.zh_CN.md Tutorial.md Tutorial.zh_CN.md LICENSE conf data
SUBDIRS = utils src tests

install-data-local:
	install -d $(DESTDIR)$(prefix)/conf $(DESTDIR)$(prefix)/data $(DESTDIR)$(prefix)/log $(DESTDIR)$(prefix)/var $(DESTDIR)$(prefix)/var/articles $(DESTDIR)$(prefix)/var/chicken $(DESTDIR)$(prefix)/var/gen_ex
	cd $(srcdir)/conf; find . -maxdepth 1 -type f -exec install -t $(DESTDIR)$(prefix)/conf -m 644 -p {} \;
	cd $(srcdir)/data; find . -maxdepth 1 -type f -exec install -t $(DESTDIR)$(prefix)/data -m 644 -p {} \;
	find $(DESTDIR)$(prefix)/ -type d -exec chmod 750 {} \;
	install -d -m 755 $(DESTDIR)$(LOGROTATE_DIR)
	install -m 644 -p conf/lbbs.logrotate $(DESTDIR)$(LOGROTATE_DIR)/lbbs
	@if test -n "$(SYSTEMD_UNIT_DIR)"; then \
		install -d -m 755 $(DESTDIR)$(SYSTEMD_UNIT_DIR) && \
		install -m 644 -p conf/lbbs.service $(DESTDIR)$(SYSTEMD_UNIT_DIR)/lbbs.service; \
	fi

uninstall-local:
	cd $(srcdir)/conf; find . -maxdepth 1 -type f -exec rm -f $(DESTDIR)$(prefix)/conf/{} \;
	cd $(srcdir)/data; find . -maxdepth 1 -type f -exec rm -f $(DESTDIR)$(prefix)/data/{} \;
	rm -f $(DESTDIR)$(LOGROTATE_DIR)/lbbs
	@if test -n "$(SYSTEMD_UNIT_DIR)"; then \
		rm -f $(DESTDIR)$(SYSTEMD_UNIT_DIR)/lbbs.service; \
	fi
