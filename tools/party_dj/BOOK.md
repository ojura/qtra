# Spomenar material for the LARICS party

The party's visual and musical references come from **Kako je LARICS postao tridesetogodišnjak**, the 2026 Spomenar written and edited by Zdenko Kovačić and published by the University of Zagreb Faculty of Electrical Engineering and Computing. The full extracted text has been read, including the history, people, projects, events, awards, thesis and publication lists, statistics, afterword, album and index. Selected page previews and every original photo in the party catalog have also been visually inspected.

The book's tone is affectionate, specific and playful. Stjepan Bogdan's foreword treats the lab as a community of people who experiment, build, cook, sail, play and make music together. Use the actual stories as the basis for short jokes. Keep invented DJ lines separate from captions that describe the historical photographs.

## Edition and local assets

- Public reader: <https://cloud.larics.fer.hr/spomenar/>
- Publisher configuration: <https://cloud.larics.fer.hr/spomenar/config.json>
- Edition observed on 17 November 2026: `publicationGeneration: 10`.
- Publisher's source SHA: `527ad3c17f4e8ab55a81a3915a6b40f4dc5b70674ccf17ab8ff7af8faf3f84b4`.
- Original PDF: <https://cloud.larics.fer.hr/spomenar-versions/527ad3c17f4e8ab55a81a3915a6b40f4dc5b70674ccf17ab8ff7af8faf3f84b4/book.pdf>
- Publisher preview root: <https://cloud.larics.fer.hr/spomenar/previews/527ad3c17f4e8ab55a81a3915a6b40f4dc5b70674ccf17ab8ff7af8faf3f84b4/>
- Catalog: `/home/juraj/.local/state/larics-dj/book/cards.json`.
- Image directory: `/home/juraj/.local/state/larics-dj/book/`.
- Compact source cache: `/home/juraj/.local/state/larics-dj/book/source/` contains `layout.pdf`, `book.txt`, `manifest.json` and `reading.json`.

This edition contains 460 physical PDF pages. For the numbered body, **physical PDF page = printed page + 11**. The viewer uses the physical number in `#page=`. For example, printed page 20 is <https://cloud.larics.fer.hr/spomenar/#page=31>. Roman front matter does not use this formula. The convention is also documented in the viewer repository's `docs/spomenar.md` and implemented by its standalone page-link configuration.

The complete 1.61 GB original was unnecessary for this task. The publisher's 2.9 MB structural layout PDF supplied all text, its page previews supplied the complete printed appearance, and `reading.json` supplied the original image resources. The structural PDF is `reading/layout-7c91cdb902fcc9c56efbd012ecb73d05947784d84c754636819a5d7c0edbc2a9.pdf` under the preview root. The two publisher indexes retain the page placement and resource mapping needed to select more images later.

The initial party selection has **77 images: 33 original photos and 44 complete page previews**, totaling 40,762,397 bytes. Prefer `kind: "photo"` for the small native stage card. Full page previews work better when a guest opens the source, or when a page's typography is itself the subject, such as LARICS/LYRICS.

Each catalog entry contains `filename`, `imagePath`, `path`, `kind`, `title`, `caption`, printed `page`, `pdfPage`, `sourceUrl`, exact publisher asset `origin`, `attribution`, `width`, `height` and `bytes`. Photo entries also carry the publisher's `resourceId`. The native stage uses `imagePath`, `title` and `page`; the guest service can expose its served image URL and `sourceUrl`. The service reads the catalog again during rotation, so replace `cards.json` atomically after adding reviewed entries.

### Credits

The imprint credits the LARICS, FER, ZARI and Foto sekcija KSET-a archives, plus Zdenko Kovačić, Stjepan Bogdan and Krešimir Petrinec. Domagoj Bogdan designed the cover. Elena Wachtler, Josip Spudić and Juraj Oršulić handled editorial revision. The 2019 KSET concert photographs on printed pages 305 to 308 and the 2022 concert collection on page 315 explicitly credit **Foto sekcija KSET-a**; that credit is included in the corresponding catalog entries. Keep visible embedded photographic credits when displaying the originals.

## Music and stage cues

The first two columns below describe the book. The last column proposes new party material.

| Printed page and source | What the book records | Suggested DJ use |
| --- | --- | --- |
| [20](https://cloud.larics.fer.hr/spomenar/#page=31) | Damjan Miklić and Zdenko Kovačić join Israeli colleagues for karaoke in Çeşme. The photograph includes a robot on stage. | Bring back the karaoke photo during a familiar singalong. The photograph's date stamp and narrative year differ, so its short caption uses the place without a year. |
| [34](https://cloud.larics.fer.hr/spomenar/#page=45) | Searches for LARICS used to suggest “Did you mean LYRICS?” The lab adopted its orange identity in 2014. | The request-search joke is already supplied by history. Orange is a grounded visual identity for the night. |
| [83](https://cloud.larics.fer.hr/spomenar/#page=94) | The 2012 sailing album includes guitar playing on Šunj beach, Lopud, alongside its joking “Horror Grill” sequence. | An acoustic or coastal interlude, with the actual guitar photo. |
| [89](https://cloud.larics.fer.hr/spomenar/#page=100) | A 2016 sailing picture is captioned as a trio of troubadours. | A three-character chorus or three-beat visual entrance. |
| [295](https://cloud.larics.fer.hr/spomenar/#page=306) | At HAZU's Noć muzeja in 2017, visitors ignored NAO reading the programme. The team switched to reciting the refrain of Azra's **Lijepe žene prolaze kroz grad**; Mirko Kokot joined in. | A particularly apt cue for adapting to the room. Use the Azra photo when that song is requested or played. |
| [296](https://cloud.larics.fer.hr/spomenar/#page=307) | Students and NAO performed a scene from Čapek's **R.U.R.** at Noć knjige; NAO also read Hamlet's “Biti ili ne biti” monologue. In 2023 Pepper read Arthur C. Clarke, hosted a quiz and closed with Asimov's laws. | The sassy soliloquy has a direct lab precedent. A brief “Biti ili ne biti u redu čekanja” caption can accompany a queue transition. |
| [300](https://cloud.larics.fer.hr/spomenar/#page=311) | Baxter served sweets as a robot Santa at the 2018 charity Advent event. | A robot can deliver the next track as a gift; the photo gives the animation context. |
| [301 to 308](https://cloud.larics.fer.hr/spomenar/#page=312) | **ROBOBAND** combined LARICS musicians with robots for a charity concert at KSET on 15 December 2019. | Use the real concert photography for the lively middle of the set. |
| [302](https://cloud.larics.fer.hr/spomenar/#page=313) | The roster includes Stjepan on vocals/guitar, Ivo and Frano on guitars, Bruno on tamburica, Robert and Matko on accordions, Alan on keyboards and Jakov Bogdan on drums. Matko coordinated human and robot musicians. | Instrument-specific callbacks are available when a guitar, accordion or drum break lands. |
| [307](https://cloud.larics.fer.hr/spomenar/#page=318) | Robert Milijaš led the audience in **Beži cujzek, beži**. Barbara Arbanas Ferreira is also pictured performing. | A documented singalong choice, with its own audience photograph. |
| [308](https://cloud.larics.fer.hr/spomenar/#page=319) | ROBOBAND had a robot drummer. | Show the actual robot during a strong drum section; an animated robot keeping time has an authentic basis. |
| [310](https://cloud.larics.fer.hr/spomenar/#page=321) | ROBOWEEK visitors met a robot xylophonist visiting from the USA. | Mallet-like particles or a brief xylophone visual can connect to this photo. |
| [311](https://cloud.larics.fer.hr/spomenar/#page=322) | The ROBOBREAK photographs include Juraj playing the event's computer game in a Santa hat. | A light personal callback when the request queue or visualizer changes level. |
| [314 to 315](https://cloud.larics.fer.hr/spomenar/#page=325) | The 2022 LARICS band was **Plava kada zaborava**. The Advent gig was held in KSET on 22 December 2022. | Use the real band name for an occasional short title card. |
| [316](https://cloud.larics.fer.hr/spomenar/#page=327) | Robert played with **Jolly Rogers** at a St. Patrick's party on 14 March 2026. Pepper appeared in Ionesco's **Ćelava pjevačica** at the Academy of Dramatic Art in March 2025. | Irish folk and robot theatre are both grounded options. The Pepper image is a particularly good bridge to qtra's performance scenes. |
| [428](https://cloud.larics.fer.hr/spomenar/#page=439) | The afterword compares the lab's development to Ravel's **Boléro**: a quiet beginning, more instruments joining, then a powerful finish. | Build a visual crescendo in layers, or reserve Boléro for a deliberately theatrical transition or closing passage. |

Additional bibliography connections: the book lists *Autonomous Robots as Actors in Robotics Theatre: Tribute to the Centenary of R.U.R.* on page 414; an electronic guitar tuner project on page 372; drone choreography projects on pages 365, 378 and 379; and *ASSISI: Charged Hot Bees Shakin' in the Spotlight* on page 408. Those are actual titles, useful for occasional nerdier callbacks.

## Robots and other grounded jokes

| Source | Actual context | Visual or caption direction |
| --- | --- | --- |
| [3](https://cloud.larics.fer.hr/spomenar/#page=14) | The lab's recorded birth is 23 October 1996 at 13:51, in room C-11-16. | A very precise birthday flash or an old-school terminal clock. |
| [5](https://cloud.larics.fer.hr/spomenar/#page=16) | Mentor, the first lab robot, arrived on 9 June 1988, before LARICS was named. | Mentor can be the elder robot character. |
| [7 to 8](https://cloud.larics.fer.hr/spomenar/#page=18) | The earlier group used the acronym FLRCG; the book jokes about its memorability. | An occasional impossible-to-pronounce announcer card. |
| [24](https://cloud.larics.fer.hr/spomenar/#page=35) | The team celebrates VIV's arrival. | Use the actual group photograph for a cheerful robot arrival. |
| [26 to 27](https://cloud.larics.fer.hr/spomenar/#page=37) | MORUS carries an underwater robot; ASSISIbf brought robots into communities of bees and fish. | A carrier delivering a smaller robot, or small agents gathering around a beat. |
| [63](https://cloud.larics.fer.hr/spomenar/#page=74) | Frkalica was played in Kovačić's office on 6 June 2005; the story jokingly casts Žagar as a VAR referee. | Board-game tokens or a mock VAR check on the next song. |
| [70](https://cloud.larics.fer.hr/spomenar/#page=81) | The fishing story inflates a tiny catch and invents an academic award for fish-school control. | Preserve its status as a joke when borrowing the exaggerated scientific language. |
| [75](https://cloud.larics.fer.hr/spomenar/#page=86) | The 2010 sailing trip on Rebeka includes Cugopol. | A board-game motif rather than an instruction to drink. |
| [94](https://cloud.larics.fer.hr/spomenar/#page=105) | Rival 2018 boat menus become an elaborate mock dispute over food and parizer thickness. | “Red čekanja narezan na 0,7 mm” is a new joke built on the documented menu story. |
| [170 to 174](https://cloud.larics.fer.hr/spomenar/#page=181) | Aerial research began with an X-UFO from Turbo Limach; RoboSapien was passed on after Bogdan's sons outgrew it. The lab acquired six NAOs. Hexapod-FP became a robot alkar. | Toy-to-research transformations, a spinning spincopter, a six-legged courier or a robot aiming for an alka. |
| [178](https://cloud.larics.fer.hr/spomenar/#page=189) | DynaRobin gives a hand in a 2013 demonstration during Alan Mutka's research. | A handshake before delivering a track. |
| [195](https://cloud.larics.fer.hr/spomenar/#page=206) | NAO demonstrates autonomous drinking from a cup in ACROSS. | A small cup-lifting motion; the three-frame source photo already reads like an animation strip. |
| [201](https://cloud.larics.fer.hr/spomenar/#page=212) | The subCULTron team and marine robots are photographed in Venice. | A gentle underwater visual interval with a real lab image. |
| [224 to 225](https://cloud.larics.fer.hr/spomenar/#page=235) | Pepper's speech recognition worked in the lab but struggled amid the noise at Weekend Media Festival. | A short joke about a robot needing the request typed during a loud party. |
| [329 to 330](https://cloud.larics.fer.hr/spomenar/#page=340) | LARICS Gripsters took third place in the ICRA 2026 human-to-robot object-handover competition using Kinova Gen3 and the lab's SofIA gripper. | “Gripsters: primopredaja refrena” is a new caption for the track-delivery robot. |
| [430](https://cloud.larics.fer.hr/spomenar/#page=441) | A Singapore travel photo plays on the transit name SMRT and its Croatian reading. | An occasional travel-sign callback. |
| [433](https://cloud.larics.fer.hr/spomenar/#page=444) | Matko, Ivica and Alan attempt “trio biciklistikus” at Matko's 2011 Fulbright farewell. | Three characters trying to share one vehicle. |
| [435](https://cloud.larics.fer.hr/spomenar/#page=446) | Breaking the Surface 2018 includes a masked ball and a guessing game about the costumes. | Let guests guess from the actual collage. |
| [437](https://cloud.larics.fer.hr/spomenar/#page=448) | The album calls a sunglasses portrait LARICS's Blues Brothers. | A blues or soul transition. |
| [439](https://cloud.larics.fer.hr/spomenar/#page=450) | The Borongaj renovation story jokes that the lab used up Zagreb's orange paint. | Let orange temporarily take over the screen. |
| [440](https://cloud.larics.fer.hr/spomenar/#page=451) | Matko made a LEGO mask for his son Luka; the book invites readers to guess who tried it on. | Show the mask as a puzzle. |
| [441](https://cloud.larics.fer.hr/spomenar/#page=452) | C-11-16 hosted a Call of Duty marathon, with a joke about the desks' remaining capacity. | A gaming interlude or a mock queue-capacity notice. |
| [445](https://cloud.larics.fer.hr/spomenar/#page=456) | The index entry for LARICS points readers to the whole book and the next edition in twenty years. | A closing joke about the next party's publication deadline. |

Useful scale, as recorded by this edition: 68 people employed over the lab's history (page 423), 97 projects (424), 13 companies founded by lab members, including four named spin-offs (425), 30 doctorates and 325 bachelor's plus 402 engineer/master's completions (349, 367, 426), and 437 publications (383 to 384, 427). Use these as book facts with their dates, not as live counters.

## Original photo selection

All filenames below are inside `/home/juraj/.local/state/larics-dj/book/`. `cards.json` is the complete mapping to exact publisher image URLs and source-page links.

| Filename | Printed page | Scene |
| --- | ---: | --- |
| `p0031-karaoke-photo.jpg` | 20 | Çeşme karaoke with a robot |
| `p0035-viv-team-photo.jpg` | 24 | Team celebrating VIV's arrival |
| `p0065-corfu-group-photo.jpg` | 54 | ICUAS 2026 group in Corfu |
| `p0074-frkalica-photo.jpg` | 63 | Frkalica in the office |
| `p0094-guitar-photo.jpg` | 83 | Guitar on Šunj beach, Lopud |
| `p0100-troubadours-photo.jpg` | 89 | Trio trubadura |
| `p0106-sailing-2018-photo.jpg` | 95 | Sailing in the Split archipelago |
| `p0184-nao-team-photo.jpg` | 173 | Researchers and NAOs |
| `p0185-robot-alkar-photo.jpg` | 174 | Hexapod-FP robot alkar |
| `p0189-dynarobin-photo.jpg` | 178 | DynaRobin handshake demonstration |
| `p0206-nao-drinks-photo.jpg` | 195 | NAO drinking, three frames |
| `p0212-venice-team-photo.jpg` | 201 | subCULTron team and robots |
| `p0306-nao-azra-photo.jpg` | 295 | NAO and Mirko performing Azra |
| `p0307-robot-theatre-photo.jpg` | 296 | Noć knjige theatre event |
| `p0311-baxter-sweets-photo.jpg` | 300 | Baxter serving sweets |
| `p0316-roboband-live-photo.jpg` | 305 | ROBOBAND and KSET audience |
| `p0317-frontman-photo.jpg` | 306 | Stjepan Bogdan performing |
| `p0317-ivo-guitar-photo.jpg` | 306 | Ivo Vatavuk on guitar |
| `p0318-barbara-vocals-photo.jpg` | 307 | Barbara Arbanas Ferreira singing |
| `p0318-robert-singalong-photo.jpg` | 307 | Robert Milijaš leading the audience |
| `p0319-robot-drummer-photo.jpg` | 308 | Robot drummer in KSET |
| `p0321-robot-xylophone-photo.jpg` | 310 | Robot xylophonist demonstration |
| `p0322-juraj-game-photo.jpg` | 311 | Juraj playing in a Santa hat |
| `p0326-plava-kada-photo.jpg` | 315 | Gathering at the 2022 Advent gig |
| `p0327-pepper-theatre-photo.jpg` | 316 | Pepper in Ćelava pjevačica |
| `p0444-bicycle-trio-photo.jpg` | 433 | Three people on one bicycle |
| `p0446-masked-ball-photo.jpg` | 435 | Breaking the Surface costumes |
| `p0447-safu-mug-photo.jpg` | 436 | I love SAFU mug |
| `p0447-gift-wrapping-photo.jpg` | 436 | Christmas gift preparations |
| `p0448-blues-brothers-photo.jpg` | 437 | LARICS's Blues Brothers |
| `p0450-orange-paint-photo.jpg` | 439 | Painting at Borongaj |
| `p0451-lego-mask-photo.jpg` | 440 | LEGO-mask guessing game |
| `p0452-call-of-duty-photo.jpg` | 441 | Gaming in C-11-16 |

## Finding more material

The source text is searchable in `source/book.txt`. Its chapter map, using printed pages, is: history 1 to 33; identity 34 to 52; social life 53 to 104; people 105 to 141; teaching 142 to 167; research and projects 168 to 275; outreach 276 to 291; events 292 to 317; awards 318 to 345; supervised work 346 to 382; publications 383 to 422; statistics 423 to 427; afterword 428; album 429 to 442; index 443 to 447.

To extend the selection, find the physical page in `source/manifest.json`. Its `assets` provide page JPEGs, and its `imageRegions` link through `resourceId` to `source/reading.json`. Download that resource from the edition's preview root, inspect its actual contents and orientation, and use the printed caption to describe it. Several original image resources extend beyond their cropped placement in the PDF, so page position alone is insufficient to identify the intended frame. Preserve the exact `origin`, correct printed page and attribution in the new catalog entry. Existing assets can be restored directly from each entry's `origin`.
