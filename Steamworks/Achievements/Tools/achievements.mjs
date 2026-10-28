// Farkle's Steam achievements: Steamworks values and the icon drawn for each.
// make-icons.mjs renders the icons; make-docs.mjs writes Achievements.md / .csv from this.
import * as L from './icon-lib.mjs';

// Stats that drive progress bars. Create these on the Stats tab first (Set By: Client).
export const stats = [
    { api: 'STAT_MATCHES_PLAYED',   name: 'Matches Played',        type: 'INT', incrementOnly: true,  min: 0, max: 1000000,   note: 'Every finished match.' },
    { api: 'STAT_BEST_WIN_STREAK',  name: 'Best Win Streak',       type: 'INT', incrementOnly: true,  min: 0, max: 1000,      note: 'Longest run of wins; only ever raised.' },
    { api: 'STAT_POINTS_BANKED',    name: 'Points Banked',         type: 'INT', incrementOnly: true,  min: 0, max: 100000000, note: 'Sum of every banked turn.' },
    { api: 'STAT_DICE_ROLLED',      name: 'Dice Rolled',           type: 'INT', incrementOnly: true,  min: 0, max: 100000000, note: 'Dice thrown, counted per die.' },
    { api: 'STAT_COMBOS_SCORED',    name: 'Combination Types Scored', type: 'INT', incrementOnly: true, min: 0, max: 8,       note: 'Distinct types: single 1, single 5, three/four/five/six of a kind, three pairs, straight.' },
    { api: 'STAT_SETTINGS_TRIED',   name: 'Settings Tried',        type: 'INT', incrementOnly: true,  min: 0, max: 30,        note: 'Distinct settings changed and applied at least once.' },
];

// cat: glow colour family (see icon-lib.mjs). stat: [api, max] for a progress bar.
export const achievements = [
    // ---------------------------------------------------------------- Rolls
    { api: 'ACH_STRAIGHT_SHOOTER', name: 'Straight Shooter', desc: 'Roll a straight: 1, 2, 3, 4, 5 and 6.', cat: 'rolls', icon: L.straight },
    { api: 'ACH_PAIR_UP', name: 'Pair Up', desc: 'Score three pairs.', cat: 'rolls', icon: L.threePairs },
    { api: 'ACH_THREE_ONES', name: 'Snake Eyes Times Three', desc: 'Score three 1s.', cat: 'rolls', icon: L.threeOnes },
    { api: 'ACH_FOUR_OF_A_KIND', name: 'Quadruple Threat', desc: 'Score four of a kind.', cat: 'rolls', icon: L.fourKind },
    { api: 'ACH_FIVE_OF_A_KIND', name: 'Handful', desc: 'Score five of a kind.', cat: 'rolls', icon: L.fiveKind },
    { api: 'ACH_SIX_OF_A_KIND', name: 'The Full Hand', desc: 'Score six of a kind.', cat: 'rolls', icon: L.sixKind },
    { api: 'ACH_SIX_ONES', name: 'One for the Ages', desc: 'Roll six 1s at once.', cat: 'rolls', icon: L.sixOnes },
    { api: 'ACH_LUCKY_FIVES', name: 'Lucky Fives', desc: 'Bank a turn in which every scoring die was a 5.', cat: 'rolls', icon: L.luckyFives },
    { api: 'ACH_HOT_DICE', name: 'Hot Hands', desc: 'Score with all six dice and roll them all again.', cat: 'rolls', icon: L.hotDice },
    { api: 'ACH_HOT_DICE_TWICE', name: 'Red Hot', desc: 'Get hot dice twice in one turn.', cat: 'rolls', icon: L.redHot },

    // ---------------------------------------------------------------- Risk and banking
    { api: 'ACH_ONE_DIE_LEFT', name: 'Pushing Your Luck', desc: 'Roll with only one die left, and score.', cat: 'risk', icon: L.oneDieLeft },
    { api: 'ACH_BANK_2000', name: 'Living Dangerously', desc: 'Bank 2,000 points or more in a single turn.', cat: 'risk', icon: L.bank2000 },
    { api: 'ACH_BANK_3000', name: 'Greed Is Good', desc: 'Bank 3,000 points or more in a single turn.', cat: 'risk', icon: L.bank3000 },
    { api: 'ACH_PLAYED_IT_SAFE', name: 'Played It Safe', desc: 'Win a match without ever rolling fewer than three dice.', cat: 'risk', icon: L.playedSafe },
    { api: 'ACH_FIFTY_AND_DONE', name: 'Fifty and Done', desc: 'Bank a turn worth exactly 50 points.', cat: 'risk', icon: L.fiftyDone },
    { api: 'ACH_SO_CLOSE', name: 'So Close', desc: 'Farkle with 1,000 points or more at risk.', cat: 'risk', icon: L.soClose },
    { api: 'ACH_HEARTBREAKER', name: 'Heartbreaker', desc: 'Farkle with enough points at risk to have won the match.', hidden: true, cat: 'risk', icon: L.heartbreaker },

    // ---------------------------------------------------------------- Farkles
    { api: 'ACH_FIRST_FARKLE', name: 'Farkle!', desc: 'Farkle for the first time.', cat: 'farkle', icon: L.firstFarkle },
    { api: 'ACH_THREE_FARKLES', name: 'Cursed Dice', desc: 'Farkle three turns in a row.', cat: 'farkle', icon: L.cursed },
    { api: 'ACH_OPENING_FARKLE', name: 'Opening Night Jitters', desc: 'Farkle on the very first roll of a match.', cat: 'farkle', icon: L.jitters },
    { api: 'ACH_ROCK_BOTTOM', name: 'Rock Bottom', desc: 'Lose a match with 0 points.', hidden: true, cat: 'farkle', icon: L.rockBottom },

    // ---------------------------------------------------------------- Winning
    { api: 'ACH_FIRST_VICTORY', name: 'First Victory', desc: 'Win a match.', cat: 'win', icon: L.firstVictory },
    { api: 'ACH_FLAWLESS', name: 'Flawless', desc: 'Win a match without farkling once.', cat: 'win', icon: L.flawless },
    { api: 'ACH_SHUTOUT', name: 'Shutout', desc: 'Win while your opponent has 0 points.', cat: 'win', icon: L.shutout },
    { api: 'ACH_COMEBACK', name: 'Comeback Kid', desc: 'Win after trailing by 1,500 points or more.', cat: 'win', icon: L.comeback },
    { api: 'ACH_NAIL_BITER', name: 'Nail-Biter', desc: 'Win by 100 points or less.', cat: 'win', icon: L.nailBiter },
    { api: 'ACH_HELD_THE_LINE', name: 'Held the Line', desc: "Win while your opponent takes their final turn to catch up.", cat: 'win', icon: L.heldLine },
    { api: 'ACH_LAST_DITCH', name: 'Last-Ditch Effort', desc: 'Overtake your opponent during your own final turn to catch up.', cat: 'win', icon: L.lastDitch },
    { api: 'ACH_SPEED_RUN', name: 'Speed Run', desc: 'Win a match in five turns or fewer.', cat: 'win', icon: L.speedRun },
    { api: 'ACH_MARATHON', name: 'Marathon', desc: 'Win with the winning score set to 10,000 or more.', cat: 'win', icon: L.marathon },
    { api: 'ACH_QUICK_DRAW', name: 'Quick Draw', desc: 'Win with the winning score set to 1,000 or less.', cat: 'win', icon: L.quickDraw },
    { api: 'ACH_HOUSE_RULES', name: 'House Rules', desc: 'Win with a custom winning score.', cat: 'win', icon: L.houseRules },

    // ---------------------------------------------------------------- Long-term
    { api: 'ACH_MATCHES_10', name: 'Regular', desc: 'Play 10 matches.', cat: 'career', stat: ['STAT_MATCHES_PLAYED', 10], icon: () => L.tankard(1) },
    { api: 'ACH_MATCHES_50', name: 'Familiar Face', desc: 'Play 50 matches.', cat: 'career', stat: ['STAT_MATCHES_PLAYED', 50], icon: () => L.tankard(2) },
    { api: 'ACH_MATCHES_100', name: 'Tavern Legend', desc: 'Play 100 matches.', cat: 'career', stat: ['STAT_MATCHES_PLAYED', 100], icon: () => L.tankard(3) },
    { api: 'ACH_STREAK_3', name: 'On a Roll', desc: 'Win 3 matches in a row.', cat: 'career', stat: ['STAT_BEST_WIN_STREAK', 3], icon: () => L.streak('3', 1) },
    { api: 'ACH_STREAK_5', name: 'Hot Streak', desc: 'Win 5 matches in a row.', cat: 'career', stat: ['STAT_BEST_WIN_STREAK', 5], icon: () => L.streak('5', 2) },
    { api: 'ACH_STREAK_10', name: 'Unstoppable', desc: 'Win 10 matches in a row.', cat: 'career', stat: ['STAT_BEST_WIN_STREAK', 10], icon: () => L.streak('10', 3) },
    { api: 'ACH_BANKER', name: 'Banker', desc: 'Bank 100,000 points across all your matches.', cat: 'career', stat: ['STAT_POINTS_BANKED', 100000], icon: L.banker },
    { api: 'ACH_COLLECTOR', name: 'Collector', desc: 'Score every type of combination at least once.', cat: 'career', stat: ['STAT_COMBOS_SCORED', 8], icon: L.collector },
    { api: 'ACH_DICE_WHISPERER', name: 'Dice Whisperer', desc: 'Roll 10,000 dice.', cat: 'career', stat: ['STAT_DICE_ROLLED', 10000], icon: L.whisperer },

    // ---------------------------------------------------------------- Hidden and funny
    { api: 'ACH_COULDNT_RESIST', name: "Couldn't Resist", desc: 'Roll again when banking would have won the match.', hidden: true, cat: 'secret', icon: L.couldntResist },
    { api: 'ACH_INDECISIVE', name: 'Indecisive', desc: 'Select and deselect the same die 10 times in one turn.', hidden: true, cat: 'secret', icon: L.indecisive },
    { api: 'ACH_NIGHT_OWL', name: 'Night Owl', desc: 'Finish a match between midnight and 4 AM.', hidden: true, cat: 'secret', icon: L.nightOwl },

    // ---------------------------------------------------------------- Lighting and path tracing
    { api: 'ACH_FOLLOW_THE_LIGHT', name: 'Follow the Light', desc: 'Win a match with Path Tracing set to Real Time.', cat: 'light', icon: L.followLight },
    { api: 'ACH_GROUND_TRUTH', name: 'Ground Truth', desc: 'Let a Reference path-traced image fully converge.', cat: 'light', icon: L.groundTruth },
    { api: 'ACH_PHOTOGRAPHER', name: 'Photographer', desc: 'Take a Steam screenshot while path tracing is on.', cat: 'light', icon: L.photographer },
    { api: 'ACH_LIGHT_AND_DARK', name: 'Light and Dark', desc: 'Finish one match with GI Quality on Off and another on Ultra.', cat: 'light', icon: L.lightDark },
    { api: 'ACH_BOUNCE_HOUSE', name: 'Bounce House', desc: 'Play a full match with path tracing on.', cat: 'light', icon: L.bounceHouse },

    // ---------------------------------------------------------------- Quality extremes
    { api: 'ACH_POTATO_MODE', name: 'Potato Mode', desc: 'Win with every quality setting on Low or Off.', cat: 'quality', icon: L.potato },
    { api: 'ACH_MAXED_OUT', name: 'Maxed Out', desc: 'Win with every quality setting on Ultra and path tracing on.', cat: 'quality', icon: L.maxedOut },
    { api: 'ACH_PURIST', name: 'Purist', desc: 'Win at native resolution with no upscaler.', cat: 'quality', icon: L.purist },
    { api: 'ACH_FULL_SPECTRUM', name: 'The Full Spectrum', desc: 'Finish a match on each quality tier: Low, Medium, High and Ultra.', cat: 'quality', icon: L.fullSpectrum },
    { api: 'ACH_LIGHTS_OUT', name: 'Lights Out', desc: 'Win with Shadows set to Off.', cat: 'quality', icon: L.lightsOut },
    { api: 'ACH_CLEAR_SKIES', name: 'Clear Skies', desc: 'Win with Volumetric Fog off, then win again with it on Ultra.', cat: 'quality', icon: L.clearSkies },
    { api: 'ACH_CONTACT_SHADOWS', name: 'Contact Shadows', desc: 'Turn Ambient Occlusion off and back on in one session.', cat: 'quality', icon: L.contactShadows },

    // ---------------------------------------------------------------- Upscalers and anti-aliasing
    { api: 'ACH_ALPHABET_SOUP', name: 'Alphabet Soup', desc: 'Finish a match with every upscaler your PC offers.', cat: 'upscale', icon: L.alphabetSoup },
    { api: 'ACH_IN_HOUSE', name: 'In-House', desc: "Win using AAR, the engine's own upscaler.", cat: 'upscale', icon: L.inHouse },
    { api: 'ACH_SHARP_EYE', name: 'Sharp Eye', desc: 'Apply every Upscaler Sharpening level.', cat: 'upscale', icon: L.sharpEye },
    { api: 'ACH_EDGE_LORD', name: 'Edge Lord', desc: 'Finish a match with each anti-aliasing mode: TAA, SMAA and MSAA.', cat: 'upscale', icon: L.edgeLord },
    { api: 'ACH_ULTRA_PERFORMANCE', name: 'Ultra Performance', desc: 'Win with an upscaler on Ultra Performance.', cat: 'upscale', icon: L.ultraPerformance },
    { api: 'ACH_TWICE_THE_FRAMES', name: 'Twice the Frames', desc: 'Win with frame generation on.', cat: 'upscale', icon: L.twiceFrames },

    // ---------------------------------------------------------------- Display, image and sound
    { api: 'ACH_WINDOW_SHOPPER', name: 'Window Shopper', desc: 'Play in all three display modes.', cat: 'display', icon: L.windowShopper },
    { api: 'ACH_BIG_SCREEN', name: 'Big Screen', desc: 'Play at 2560 × 1440 or higher.', cat: 'display', icon: L.bigScreen },
    { api: 'ACH_THROUGH_THE_LENS', name: 'Through the Lens', desc: 'Win with Chromatic Aberration off, then win again with it on.', cat: 'display', icon: L.throughLens },
    { api: 'ACH_NUMBERS_PERSON', name: 'Numbers Person', desc: 'Play a full match with Show FPS & Latency on.', cat: 'display', icon: L.numbersPerson },
    { api: 'ACH_BENCHMARK', name: 'Benchmark', desc: 'Average 60 FPS or more over a match with path tracing on.', cat: 'display', icon: L.benchmark },
    { api: 'ACH_SILENT_TREATMENT', name: 'Silent Treatment', desc: 'Win with every volume at 0.', cat: 'display', icon: L.silent },
    { api: 'ACH_JUST_THE_MUSIC', name: 'Just the Music', desc: 'Win with Sound at 0 and Music at 100.', cat: 'display', icon: L.justMusic },

    // ---------------------------------------------------------------- Meta
    { api: 'ACH_TINKERER', name: 'Tinkerer', desc: 'Change and apply 10 different settings.', cat: 'meta', stat: ['STAT_SETTINGS_TRIED', 10], icon: L.tinkerer },
    { api: 'ACH_BACK_TO_BASICS', name: 'Back to Basics', desc: 'Restore the default settings after unlocking five settings achievements.', cat: 'meta', icon: L.backToBasics },
    { api: 'ACH_GRAPHICS_CONNOISSEUR', name: 'Graphics Connoisseur', desc: 'Unlock every settings achievement that any PC can earn.', cat: 'meta', icon: L.connoisseur },
    { api: 'ACH_COMPLETIONIST', name: 'Completionist', desc: 'Unlock every gameplay achievement.', cat: 'meta', icon: L.completionist },
    { api: 'ACH_FARKLE_MASTER', name: 'Farkle Master', desc: 'Unlock every achievement your PC can earn.', cat: 'meta', icon: L.farkleMaster },
];
