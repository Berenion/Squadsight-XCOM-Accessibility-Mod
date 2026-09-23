# Credits

An accessibility mod for XCOM: Enemy Unknown and XCOM: Enemy Within, so the
game can be played without sight. It is unofficial and not affiliated with
Firaxis Games or 2K. XCOM is a trademark of Take-Two Interactive Software.

## Sounds

- **Alien heartbeat** (`alienbeat.wav`): "sinth_heart" by **renzogen**,
  Freesound 351789, https://freesound.org/people/renzogen/sounds/351789/ .
  Licence: Creative Commons Attribution 4.0 (CC BY 4.0). Cut to one beat.
- **Door sound** (`doorsound.wav`): "doors wood door with latch 1" by
  **pnmcarrierailfan**, Freesound 684538,
  https://freesound.org/people/pnmcarrierailfan/sounds/684538/ . Licence:
  Creative Commons Attribution-NonCommercial 4.0 (CC BY-NC 4.0). Cut and
  converted from MP3. Because of this licence, the mod must not be sold or
  included in anything sold while this sound is part of it.
- **Window sound** (`windowsound.wav`): "window shut" by **dilly_deelin**,
  Freesound 784299,
  https://freesound.org/people/dilly_deelin/sounds/784299/ . Licence: CC0
  (public domain); credited with thanks.
- The ally beep, the height-change tones and the wall sound are generated
  by the mod.

## Software the mod includes

- **MinHook**, by Tsuda Kageyu, https://github.com/TsudaKageyu/minhook ,
  which contains the **Hacker Disassembler Engine 32 C** by Vyacheslav
  Patkov. BSD 2-Clause licence; the full notice is at the end of this file.
- **Tolk**, by Davy Kager, https://github.com/dkager/tolk : speech through
  whichever screen reader is running. GNU Lesser General Public License
  v3. It ships unmodified, as its own DLL.
- **NVDA Controller Client**, by NV Access, https://www.nvaccess.org/ :
  speech through NVDA. GNU Lesser General Public License v2.1. It ships
  unmodified, as its own DLL. The source for both libraries is at the
  addresses above.

## Work this mod learned from

- **NonVisualCalculus**, by Rashad Naqeeb (MIT licence): the shape of the
  sound mixer (one mixer, voices on it, a soft limiter across the output)
  is taken from it. The code is written anew; none of it is copied.
- **The Wasteland 2 accessibility mod**, by **Berenion**, who also made
  this one: the scanner's design and its keys (Page Up and Page Down, Home,
  End), kept the same so a player who has both does not learn two schemes.
- **xcom2access**, by **alex19EP**, https://git.alex19ep.me/alex19EP/xcom2access
  , the XCOM 2 accessibility mod: several features were modelled on its
  work.

## Tools used to build it

- **UELib (Unreal Library)**, by Eliot van Uytfanghe,
  https://github.com/EliotVU/Unreal-Library (MIT licence): used to read the
  game's scripts while writing the mod. It is not part of the mod.

---

## MinHook licence

Reproduced verbatim from MinHook's `LICENSE.txt`:

```
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

================================================================================
Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.
================================================================================
Hacker Disassembler Engine 32 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------
Hacker Disassembler Engine 64 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
