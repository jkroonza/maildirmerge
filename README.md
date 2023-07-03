# maildirtools
Collection of tools to manipulate maildir mail structures.

## maildirarchive
Tool to archive emails from a maildir into alternative mail dirs.  Important to
note that archiving is done based on timestamp in the filename, which may not
be what you want.  You may want to use maildirdate2filename to rename files
first.  Another future idea may be to use --header option to actually read the
file contents, however, this would be super inefficient in the long term (We
run this on ~3TB worth of email once a week).

## maildircheck
Script to find faults based on the maildir spec as per
http://cr.yp.to/proto/maildir.html - incorporating a few "quirks" as discovered
by reading various spec files.  There are probably more checks that can be
added.

Has also learned the ability to fix certain errors (-F - use with care, whilst
all possible care has been taken I cannot guarantee the absense of bugs, but
this worked very well for us).

## maildirdate2filename
Tool to read all the headers for emails in a specific folder and ensure that
the timestamp in the filename correlates with the Date: header.

## maildirmerge
Was written as a kind of "quick fix" to emrge maildirs semi intelligently.  The
idea here was that we've got a mix of IMAP and POP3 users, a domain migrated
and we ended up in a situation where users had two separate mailboxes that
needed to be merged.  The normal strategy is to just copy all mail over the
existing maildir, but this has a couple of drawbacks:

1.  POP3 users will re-download everything in INBOX - in our case the largest count here would have been ~140k emails.
2.  IMAP users will also re-download everything, several TB in total here due to the uidb databases getting clobbered.

## maildirsizes
Very simple tool to deduce the maildir size from the filenames.

Essentially it will just do recursive readdir() to extract filenames and just
sum it all up, outputting per folder and totals (depending on arguments given).

## maildirreconstruct
Given multiple folders each with different "snippets" of the same maildir, reconstruct the maildir as far as is possible.  We had
a 3x2 glusterfs distribute-replicate filesystem that picked up some problems, and this was used to reconstruct the mailboxes from
the 6 bricks back onto a clean filesystem.  So far as we can determine this worked flawlessly, but again, please keep your original
data in tact (and backup it) before using this.

## maildirduperem
Particularly nasty piece of shell script to iterate a mailbox, finding duplicate files and removing the duplicates.  This was
written because we had one user where Outlook decided to repeatedly copy the same email from IMBOX. into a subfolder ... from
<5GB to over 1TB in a day ...
