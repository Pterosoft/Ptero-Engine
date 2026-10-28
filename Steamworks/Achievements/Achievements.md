# Farkle — Steamworks achievements

Generated from `Tools/achievements.mjs`; run `Tools/render.ps1` after editing it.

## 1. Stats (Stats & Achievements → Stats tab — create these first)

| API Name | Display Name | Set By | Type | Increment Only | Min | Max | Default | Notes |
|---|---|---|---|---|---|---|---|---|
| `STAT_MATCHES_PLAYED` | Matches Played | Client | INT | Yes | 0 | 1000000 | 0 | Every finished match. |
| `STAT_BEST_WIN_STREAK` | Best Win Streak | Client | INT | Yes | 0 | 1000 | 0 | Longest run of wins; only ever raised. |
| `STAT_POINTS_BANKED` | Points Banked | Client | INT | Yes | 0 | 100000000 | 0 | Sum of every banked turn. |
| `STAT_DICE_ROLLED` | Dice Rolled | Client | INT | Yes | 0 | 100000000 | 0 | Dice thrown, counted per die. |
| `STAT_COMBOS_SCORED` | Combination Types Scored | Client | INT | Yes | 0 | 8 | 0 | Distinct types: single 1, single 5, three/four/five/six of a kind, three pairs, straight. |
| `STAT_SETTINGS_TRIED` | Settings Tried | Client | INT | Yes | 0 | 30 | 0 | Distinct settings changed and applied at least once. |

## 2. Achievements (74)

Every achievement: **Set By = Client**. Icons: `Icons/achieved/<API Name>.png` and `Icons/unachieved/<API Name>.png` (256 × 256).
Progress Stat "None" means leave Min/Max at 0.

| # | API Name | Display Name | Description | Hidden | Progress Stat | Min | Max |
|---|---|---|---|---|---|---|---|
| 1 | `ACH_STRAIGHT_SHOOTER` | Straight Shooter | Roll a straight: 1, 2, 3, 4, 5 and 6. |  | None | 0 | 0 |
| 2 | `ACH_PAIR_UP` | Pair Up | Score three pairs. |  | None | 0 | 0 |
| 3 | `ACH_THREE_ONES` | Snake Eyes Times Three | Score three 1s. |  | None | 0 | 0 |
| 4 | `ACH_FOUR_OF_A_KIND` | Quadruple Threat | Score four of a kind. |  | None | 0 | 0 |
| 5 | `ACH_FIVE_OF_A_KIND` | Handful | Score five of a kind. |  | None | 0 | 0 |
| 6 | `ACH_SIX_OF_A_KIND` | The Full Hand | Score six of a kind. |  | None | 0 | 0 |
| 7 | `ACH_SIX_ONES` | One for the Ages | Roll six 1s at once. |  | None | 0 | 0 |
| 8 | `ACH_LUCKY_FIVES` | Lucky Fives | Bank a turn in which every scoring die was a 5. |  | None | 0 | 0 |
| 9 | `ACH_HOT_DICE` | Hot Hands | Score with all six dice and roll them all again. |  | None | 0 | 0 |
| 10 | `ACH_HOT_DICE_TWICE` | Red Hot | Get hot dice twice in one turn. |  | None | 0 | 0 |
| 11 | `ACH_ONE_DIE_LEFT` | Pushing Your Luck | Roll with only one die left, and score. |  | None | 0 | 0 |
| 12 | `ACH_BANK_2000` | Living Dangerously | Bank 2,000 points or more in a single turn. |  | None | 0 | 0 |
| 13 | `ACH_BANK_3000` | Greed Is Good | Bank 3,000 points or more in a single turn. |  | None | 0 | 0 |
| 14 | `ACH_PLAYED_IT_SAFE` | Played It Safe | Win a match without ever rolling fewer than three dice. |  | None | 0 | 0 |
| 15 | `ACH_FIFTY_AND_DONE` | Fifty and Done | Bank a turn worth exactly 50 points. |  | None | 0 | 0 |
| 16 | `ACH_SO_CLOSE` | So Close | Farkle with 1,000 points or more at risk. |  | None | 0 | 0 |
| 17 | `ACH_HEARTBREAKER` | Heartbreaker | Farkle with enough points at risk to have won the match. | ☑ | None | 0 | 0 |
| 18 | `ACH_FIRST_FARKLE` | Farkle! | Farkle for the first time. |  | None | 0 | 0 |
| 19 | `ACH_THREE_FARKLES` | Cursed Dice | Farkle three turns in a row. |  | None | 0 | 0 |
| 20 | `ACH_OPENING_FARKLE` | Opening Night Jitters | Farkle on the very first roll of a match. |  | None | 0 | 0 |
| 21 | `ACH_ROCK_BOTTOM` | Rock Bottom | Lose a match with 0 points. | ☑ | None | 0 | 0 |
| 22 | `ACH_FIRST_VICTORY` | First Victory | Win a match. |  | None | 0 | 0 |
| 23 | `ACH_FLAWLESS` | Flawless | Win a match without farkling once. |  | None | 0 | 0 |
| 24 | `ACH_SHUTOUT` | Shutout | Win while your opponent has 0 points. |  | None | 0 | 0 |
| 25 | `ACH_COMEBACK` | Comeback Kid | Win after trailing by 1,500 points or more. |  | None | 0 | 0 |
| 26 | `ACH_NAIL_BITER` | Nail-Biter | Win by 100 points or less. |  | None | 0 | 0 |
| 27 | `ACH_HELD_THE_LINE` | Held the Line | Win while your opponent takes their final turn to catch up. |  | None | 0 | 0 |
| 28 | `ACH_LAST_DITCH` | Last-Ditch Effort | Overtake your opponent during your own final turn to catch up. |  | None | 0 | 0 |
| 29 | `ACH_SPEED_RUN` | Speed Run | Win a match in five turns or fewer. |  | None | 0 | 0 |
| 30 | `ACH_MARATHON` | Marathon | Win with the winning score set to 10,000 or more. |  | None | 0 | 0 |
| 31 | `ACH_QUICK_DRAW` | Quick Draw | Win with the winning score set to 1,000 or less. |  | None | 0 | 0 |
| 32 | `ACH_HOUSE_RULES` | House Rules | Win with a custom winning score. |  | None | 0 | 0 |
| 33 | `ACH_MATCHES_10` | Regular | Play 10 matches. |  | `STAT_MATCHES_PLAYED` | 0 | 10 |
| 34 | `ACH_MATCHES_50` | Familiar Face | Play 50 matches. |  | `STAT_MATCHES_PLAYED` | 0 | 50 |
| 35 | `ACH_MATCHES_100` | Tavern Legend | Play 100 matches. |  | `STAT_MATCHES_PLAYED` | 0 | 100 |
| 36 | `ACH_STREAK_3` | On a Roll | Win 3 matches in a row. |  | `STAT_BEST_WIN_STREAK` | 0 | 3 |
| 37 | `ACH_STREAK_5` | Hot Streak | Win 5 matches in a row. |  | `STAT_BEST_WIN_STREAK` | 0 | 5 |
| 38 | `ACH_STREAK_10` | Unstoppable | Win 10 matches in a row. |  | `STAT_BEST_WIN_STREAK` | 0 | 10 |
| 39 | `ACH_BANKER` | Banker | Bank 100,000 points across all your matches. |  | `STAT_POINTS_BANKED` | 0 | 100000 |
| 40 | `ACH_COLLECTOR` | Collector | Score every type of combination at least once. |  | `STAT_COMBOS_SCORED` | 0 | 8 |
| 41 | `ACH_DICE_WHISPERER` | Dice Whisperer | Roll 10,000 dice. |  | `STAT_DICE_ROLLED` | 0 | 10000 |
| 42 | `ACH_COULDNT_RESIST` | Couldn't Resist | Roll again when banking would have won the match. | ☑ | None | 0 | 0 |
| 43 | `ACH_INDECISIVE` | Indecisive | Select and deselect the same die 10 times in one turn. | ☑ | None | 0 | 0 |
| 44 | `ACH_NIGHT_OWL` | Night Owl | Finish a match between midnight and 4 AM. | ☑ | None | 0 | 0 |
| 45 | `ACH_FOLLOW_THE_LIGHT` | Follow the Light | Win a match with Path Tracing set to Real Time. |  | None | 0 | 0 |
| 46 | `ACH_GROUND_TRUTH` | Ground Truth | Let a Reference path-traced image fully converge. |  | None | 0 | 0 |
| 47 | `ACH_PHOTOGRAPHER` | Photographer | Take a Steam screenshot while path tracing is on. |  | None | 0 | 0 |
| 48 | `ACH_LIGHT_AND_DARK` | Light and Dark | Finish one match with GI Quality on Off and another on Ultra. |  | None | 0 | 0 |
| 49 | `ACH_BOUNCE_HOUSE` | Bounce House | Play a full match with path tracing on. |  | None | 0 | 0 |
| 50 | `ACH_POTATO_MODE` | Potato Mode | Win with every quality setting on Low or Off. |  | None | 0 | 0 |
| 51 | `ACH_MAXED_OUT` | Maxed Out | Win with every quality setting on Ultra and path tracing on. |  | None | 0 | 0 |
| 52 | `ACH_PURIST` | Purist | Win at native resolution with no upscaler. |  | None | 0 | 0 |
| 53 | `ACH_FULL_SPECTRUM` | The Full Spectrum | Finish a match on each quality tier: Low, Medium, High and Ultra. |  | None | 0 | 0 |
| 54 | `ACH_LIGHTS_OUT` | Lights Out | Win with Shadows set to Off. |  | None | 0 | 0 |
| 55 | `ACH_CLEAR_SKIES` | Clear Skies | Win with Volumetric Fog off, then win again with it on Ultra. |  | None | 0 | 0 |
| 56 | `ACH_CONTACT_SHADOWS` | Contact Shadows | Turn Ambient Occlusion off and back on in one session. |  | None | 0 | 0 |
| 57 | `ACH_ALPHABET_SOUP` | Alphabet Soup | Finish a match with every upscaler your PC offers. |  | None | 0 | 0 |
| 58 | `ACH_IN_HOUSE` | In-House | Win using AAR, the engine's own upscaler. |  | None | 0 | 0 |
| 59 | `ACH_SHARP_EYE` | Sharp Eye | Apply every Upscaler Sharpening level. |  | None | 0 | 0 |
| 60 | `ACH_EDGE_LORD` | Edge Lord | Finish a match with each anti-aliasing mode: TAA, SMAA and MSAA. |  | None | 0 | 0 |
| 61 | `ACH_ULTRA_PERFORMANCE` | Ultra Performance | Win with an upscaler on Ultra Performance. |  | None | 0 | 0 |
| 62 | `ACH_TWICE_THE_FRAMES` | Twice the Frames | Win with frame generation on. |  | None | 0 | 0 |
| 63 | `ACH_WINDOW_SHOPPER` | Window Shopper | Play in all three display modes. |  | None | 0 | 0 |
| 64 | `ACH_BIG_SCREEN` | Big Screen | Play at 2560 × 1440 or higher. |  | None | 0 | 0 |
| 65 | `ACH_THROUGH_THE_LENS` | Through the Lens | Win with Chromatic Aberration off, then win again with it on. |  | None | 0 | 0 |
| 66 | `ACH_NUMBERS_PERSON` | Numbers Person | Play a full match with Show FPS & Latency on. |  | None | 0 | 0 |
| 67 | `ACH_BENCHMARK` | Benchmark | Average 60 FPS or more over a match with path tracing on. |  | None | 0 | 0 |
| 68 | `ACH_SILENT_TREATMENT` | Silent Treatment | Win with every volume at 0. |  | None | 0 | 0 |
| 69 | `ACH_JUST_THE_MUSIC` | Just the Music | Win with Sound at 0 and Music at 100. |  | None | 0 | 0 |
| 70 | `ACH_TINKERER` | Tinkerer | Change and apply 10 different settings. |  | `STAT_SETTINGS_TRIED` | 0 | 10 |
| 71 | `ACH_BACK_TO_BASICS` | Back to Basics | Restore the default settings after unlocking five settings achievements. |  | None | 0 | 0 |
| 72 | `ACH_GRAPHICS_CONNOISSEUR` | Graphics Connoisseur | Unlock every settings achievement that any PC can earn. |  | None | 0 | 0 |
| 73 | `ACH_COMPLETIONIST` | Completionist | Unlock every gameplay achievement. |  | None | 0 | 0 |
| 74 | `ACH_FARKLE_MASTER` | Farkle Master | Unlock every achievement your PC can earn. |  | None | 0 | 0 |
