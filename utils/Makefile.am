EXTRA_DIST = bin conf lib
SUBDIRS=game

install-exec-local:
	install -d $(DESTDIR)$(prefix)/$(subdir)/bin $(DESTDIR)$(prefix)/$(subdir)/lib
	cd $(srcdir)/bin; find . -maxdepth 1 -type f -exec install -t $(DESTDIR)$(prefix)/$(subdir)/bin {} \;
	cd $(srcdir)/lib; find . -maxdepth 1 -type f -exec install -t $(DESTDIR)$(prefix)/$(subdir)/lib {} \;

install-data-local:
	install -d $(DESTDIR)$(prefix)/$(subdir)/conf
	cd $(srcdir)/conf; find . -maxdepth 1 -type f -exec install -t $(DESTDIR)$(prefix)/$(subdir)/conf {} \;

uninstall-local:
	cd $(srcdir)/bin; find . -type f -exec rm -f $(DESTDIR)$(prefix)/$(subdir)/bin/{} \;
	cd $(srcdir)/conf; find . -type f -exec rm -f $(DESTDIR)$(prefix)/$(subdir)/conf/{} \;
	cd $(srcdir)/lib; find . -type f -exec rm -f $(DESTDIR)$(prefix)/$(subdir)/lib/{} \;
