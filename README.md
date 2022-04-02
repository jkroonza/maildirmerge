# maildirtools
Collection of tools to manipulate maildir mail structures.

## maildirmerge
Was written as a kind of "quick fix" to emrge maildirs semi intelligently.  The idea here was that we've got a mix of IMAP and POP3 users, a domain migrated and we ended up in a situation where users had two separate mailboxes that needed to be merged.  The normal strategy is to just copy all mail over the existing maildir, but this has a couple of drawbacks:

1.  POP3 users will re-download everything in INBOX - in our case the largest count here would have been ~140k emails.
2.  IMAP users will also re-download everything, several TB in total here due to the uidb databases getting clobbered.
