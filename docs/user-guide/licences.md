# Licences

A licence is one signed file with the extension `.vsslic`. Studio checks it on
this computer and never contacts a server. With no licence, Studio is the
Community edition, which is complete for capturing, monitoring and sharing
serial ports. A Pro or Enterprise licence adds official plugins, the
Microsoft-signed driver and support. A licence never limits ports, setups,
capture or files.

## Help → Licence

**Help → Licence...** shows which edition this is and what the licence
includes: who it was issued to, its number, its term, its state (Valid, Ends
soon, Ended, Not started, Check the date or Not valid; with no licence it reads
Community), the seats (**Site licence** when there is no count) and, when the
licence names computers, which ones. A seat is two computers: the one you work at
and one other that you control or monitor from it. The serial device can be on
either. The state reads Ended during the grace period too, and the line under
it then says until when official plugins keep working. A table lists what the
edition includes (official plugins, the Microsoft-signed driver and support) and says
for each whether it is available and, when it is not, why.

The edition chip at the right of the status bar opens the same page. It reads
Community, Pro, Pro trial or Enterprise, and counts the days left when a licence
is about to end, as in Pro, 12 days left. In the grace period it reads, for
example, Pro, licence ended.

## Importing a licence

Choose **Import licence...** on the Licence page and pick the file, or drop the
`.vsslic` file on the page. Studio checks the signature. Only a file whose
signature is good, that was not withdrawn and that does not name other computers
is copied to your licence folder, and the file you chose stays where it was. A
file that is not valid is refused in **Could not import the licence**, with the
reason and **Choose another file**, and nothing changes. A file that is the same
as one already installed is not copied again, and Studio says **That licence is
already installed.** An import takes effect at once, with no restart. Importing
never replaces or deletes an older file: the new file gets a name of its own. A
licence that has already ended is copied, and the page says that it does not turn
anything on; one that starts later says when it starts.

## Where licences are kept

| Folder | Where | Used for |
|---|---|---|
| Your licence folder, Windows | `%LOCALAPPDATA%\Virtual Serial Studio\licences` | Where **Import licence...** copies files |
| Your licence folder, Linux | `$XDG_DATA_HOME/virtual-serial-studio/licences`, or `~/.local/share/virtual-serial-studio/licences` when `XDG_DATA_HOME` is not set | The same |
| Your licence folder, macOS | `~/Library/Application Support/Virtual Serial Studio/licences` | The same |
| The per-machine folder, Windows | `%ProgramData%\Virtual Serial Studio\licences` | A licence an administrator puts there for every user of the computer |
| The per-machine folder, Linux | `/etc/virtual-serial-studio/licences` | The same |
| The per-machine folder, macOS | `/Library/Application Support/Virtual Serial Studio/licences` | The same |

Studio reads every `.vsslic` file in both folders and uses the one that gives
the most: a valid licence before one in its grace period, that before one that
has ended, then Enterprise before Pro, then the one that ends last. The others
are listed on the Licence page. Studio only reads the per-machine folder. It
never writes to it, and **Remove** does not touch a file there.

## From the command line

`vss_cli licence` prints what Studio sees on this computer, one line for each
fact: `Edition:` (`community`, `pro` or `enterprise`), `State:` (`none`,
`valid`, `expiring`, `grace`, `expired`, `not_yet_valid`, `clock_suspect` or
`invalid`), `Reason:` when the licence is not in force, then, when there is a
licence, `Licence:` (its number), `Kind:` (`trial`, `annual` or `enterprise`),
`Licensed to:`, `Term:` (two dates), `Days left:` (when it is in force) and
`Seats:` (a number, or `site licence`). The last part is `Files:`, which is
`none` or has a line for each licence file found in the two folders, with its
state and `machine` or `user`, and `in use` for the one Studio uses. The edition
and state words are the ones the control gateway's status uses. It reads files
and sends nothing, its exit status is 0 whatever it finds, and it takes no
options.

`vss_cli licence import <file>` checks a licence file as Studio does and copies
it to your licence folder. It prints one sentence. It exits with 0 when the
licence is installed, and also when the same file is already installed and
nothing was copied, and with 1 when the file is refused, cannot be read or
cannot be copied. The file you give it stays where it was. Any other
arguments give exit code 2 and the usage text.

## When a licence ends

For the last 30 days of a term (3 days for a trial) the chip counts the days
left, and a notice at the top of the window says when the licence ends.
**Dismiss** hides it until 7 days before the end, and then until the last day.
**Licence**, on the notice, opens the Licence page.

An annual or Enterprise licence then has a grace period, normally 28 days.
Official plugins keep working, and the notice says how many days are left. It
cannot be dismissed for good: **Remind me tomorrow** brings it back the next
day. A trial has no grace period.

After that Studio is the Community edition, and a notice says so once. Your
ports, setups and files are not lost or locked, and they open and save as
before.

A licence can also carry a date for new releases (a version window). After such
a licence has ended, an official plugin whose release date is on or before that
date keeps working, and a plugin released later needs a current licence. A
licence without the date ends as described above. Annual licences are issued
without it.

## Renewing

A licence is renewed by installing a new licence file. Import it as above. It
takes effect at once. Studio opens no web page and sends nothing. **How to
renew**, on the Licence and Support pages, opens **Renew your licence**, a small
window with two buttons: **Copy address** copies the address to write to, and
**Copy licence number** copies your licence number, which you quote when you
write.

## Removing a licence

**Remove licence** on the Licence page asks first, then takes the licence out
of your licence folder. Studio returns to the Community edition. The file you
imported from is not touched, so you can import it again. The button shows only
when a licence from your own folder is in use. When more than one file is
installed, each one in the list of other licences has its own **Remove**. A
licence the administrator installed has none, and the list marks it **Installed
by an administrator**.

## When the date looks wrong

Studio keeps the latest time it has seen. It also knows when it was built and
when the licence was issued. If the date on this computer is more than 48 hours
before the latest of those times, the state is Check the date, a notice says so,
and official plugins are off until the date is right. **Dismiss** on the notice
hides it until the next day. The 48 hours allow for
time zones, daylight saving and a computer that keeps local time. Set the date
right and the warning goes. If the date is already right, create a support
bundle (**File → Support bundle**) and contact support, who can send a new
licence file that sets Studio's record of the time right.
