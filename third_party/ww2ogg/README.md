# ww2ogg packed codebooks

`packed_codebooks_aoTuV_603.bin` is the external Vorbis codebook library that Wwise-encoded
Vorbis streams index by number (10-bit codebook ids in the setup packet). It comes from
ww2ogg by hcs (https://github.com/hcs64/ww2ogg), BSD 3-clause licence, which derived the books
from the aoTuV 6.03 encoder. It contains no game data.

`tools/re/gen_codebooks.py` turns it into `src/content/wwise_codebooks.inc`.

Copyright (c) 2002, Xiph.org Foundation (codebooks); ww2ogg (c) 2009-2011 hcs.
Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the BSD 3-clause conditions are met: retain this notice in source and binary
distributions, and do not use the names of the authors to endorse derived products.
