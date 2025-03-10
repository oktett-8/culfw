# culfw for cul devices
___
**The original firmware tree is cloned from [SourceForge](https://sourceforge.net/p/culfw/code/commit_browser).**

_This fork is intended to upgrade the original culfw repository to the firmware found at [SourceForge](https://sourceforge.net/p/culfw/code/commit_browser) to be used for future patches. Original purpose is the fix of nanoCUL crashes for **wMBus**. However, the master branch contains the complete source tree up to r571 at SourceForge._

_The r568 firmware found at [wmbusmeters](https://github.com/wmbusmeters/wmbusmeters-wiki/blob/master/nanoCUL.md) (nanoCUL\_r568\_mbus\_c1t1\_bufsize300.zip) crashes some clone nanoCULs randomly (having ATmega328P/ATmega328PB CPU, but not the original Arduino Nano) while receiving some large telegrams. This is apparently caused by issues in linked avr-libc before version 2.1.0._

_This version cross-compiles well with avr-gcc version 14.2.0 against avr-libc version 2.1.0 and fixes the issue. It was also stress tested to survive wmbusmeters resets using "resetafter=1m". It also showed no issues for very long wmbusmeters reset intervals._

A flashable nanoCUL (868 MHz) for wMBus firmware with increased receive buffer size (TTY_BUFSIZE parameter in board.h, see board.h.wmbus) to 300 bytes, cross-compiled with avr-gcc version 14.2.0 against avr-libc version 2.1.0 can be found in the release binary "nanoCUL868_r571_mbus_c1t1_bufsize300_libc210.hex".
