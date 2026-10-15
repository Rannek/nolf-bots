NOLF BOTS @VERSION@
==================
Computer players for No One Lives Forever multiplayer: play H.A.R.M. vs UNITY
or DM on your own, against and alongside bots. Made for Hydro, where
they know the zipcords, the water routes and the documents. Other maps work
too, but the bots haven't been tuned for them.

The bots play fair: they know the map, but not where you are. They see, hear
and call each other out like players do, and pick up every weapon, armor and
SCUBA gear themselves.


WHAT YOU NEED
-------------
 - No One Lives Forever, Game of the Year edition
 - NOLF Modernizer 1.006, installed and working:
   https://haekb.itch.io/nolf-modernizer


INSTALL
-------
 1. Unpack this zip into your NOLF folder (the one with NOLF.exe), and let it
    merge the folders.
 2. Start NOLF.exe, click "Advanced", then "Customize".
 3. "Add" BOTS.REZ, so it is AFTER (below) Modernizer.rez in the list.
    Keep "Always load these rez files" ticked and click "Ok".


PLAY
----
 Multiplayer > Host.  Choose H.A.R.M. vs UNITY and Hydro, then start.
 Bots join within a few seconds and fill the game up to 12 players.
 On a map they haven't seen before, the bots first measure it and remember it
 for next time, so the first game there takes a moment longer to get going.


SETTINGS
--------
 Bots\bots.cfg appears the first time you host.  Open it with Notepad: every
 line explains itself.  Delete it to get the defaults back.

 The defaults:
   - everyone runs 50% faster than in the original game
   - weapons and items come back 3x faster
   - no falling damage
   - everyone spawns with a Walther SMG
   - bots fill the game to 12 players and leave as people join
   - bot skill 4 of 5, adjusting itself to keep the score close

 These replace the Host menu's run speed, item respawn, falling damage and
 default weapon options.


CHAT COMMANDS (while you host, type them in the chat)
-------------
 !bots                        list the commands
 !addbot                      one more bot   (!addbot Volkov 5 rusher)
 !removebot  /  !removebot all
 !bot_skill 3                 1 = easy ... 5 = hardest
 !bot_minplayers 8            fill the game up to 8 players
 !bot_autoskill 0             fixed skill, no balancing
 !set RunSpeed 1              any setting from bots.cfg


UNINSTALL
---------
 Advanced > Customize: remove BOTS.REZ.  Then delete Custom\BOTS.REZ and the
 Bots folder.


THE BOTS FOLDER
---------------
 bots.cfg                     your settings
 <map>.bwm, <map>.nav         what the bots measured and learned about a map
 genome.txt                   their trained fighting style
 games\                       what every bot did and why, in your last 30 games
 botlog.txt, <map>-*.txt      details for bug reports

 Something odd?  Send the log of that game from Bots\games and say what you
 saw and roughly when.


CREDITS AND LICENSE
-------------------
 Built on NOLF Modernizer by haekb (https://github.com/haekb/nolf1-modernizer),
 which is built on Monolith Productions' No One Lives Forever Source Code
 v1.003.  Free and non-commercial, covered by that source code's end-user
 license agreement.  You need your own copy of the game.
