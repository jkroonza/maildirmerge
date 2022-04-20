TARGET_BINS=maildirmerge maildirsizes maildircheck

server_types=servertypes server_courier

MODS_maildirmerge=maildirmerge $(server_types)
MODS_maildirsizes=maildirsizes
MODS_maildircheck=maildircheck

include Makefile.inc
