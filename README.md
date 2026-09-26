# Bot Shakedown

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module for
[mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) groups. Say **`cloth`** in party
or raid chat, and every bot in your group hands you the cloth in its bags.

- **No trade windows.** The cloth moves straight into your bags, merging into your stacks, the same
  way a finished trade moves items. You don't have to open, fill and accept a trade with each bot.
- **You see what you got.** One chat line per bot, for example
  `Brakka gives you 12x [Linen Cloth], 4x [Wool Cloth].`
- **Your bags filling up is handled.** The hand-over stops, and you're told which bots still have
  cloth for you. Make room and say `cloth` again.
- **The keyword has to be the whole message.** `cloth` works. `anyone got cloth?` does nothing.
  Capitals and extra spaces don't matter.

Other trade goods can have their own keyword: `leather`, `ore` (metal and stone), `meat`, `herb`,
`elemental` and `enchanting` (dusts, essences, shards). They're off by default. Turn them on in
the config.

## Who gives what

- Only playerbots give. Real players in your group are never touched.
- A bot gives only to the real player who said the keyword, and only while you're both on the same
  map, for example in the same dungeon.
- A bot keeps anything a trade window would refuse to trade: soulbound items, and items it's in
  the middle of looting. Gray junk that Blizzard's data files as trade goods also stays put.

In a group with several real players, every bot gives to whoever says the keyword.

On stock AzerothCore, without playerbots, there are no bots and the module does nothing.

## Installation

Clone it into your AzerothCore `modules` folder, **as `mod-bot-shakedown`**. The folder name matters,
because AzerothCore derives the module's loader name from it:

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/buildthehomelab/wow-mod-bot-shakedown.git mod-bot-shakedown
```

Re-run CMake, rebuild the worldserver, and copy `conf/mod_bot_shakedown.conf.dist` to
`mod_bot_shakedown.conf` in your config directory. The module needs no SQL.

It doesn't change mod-playerbots and doesn't need its headers. It recognizes bots by the
`WorldSession::IsBot()` that the playerbots core fork adds.

## Configuration

| Option | Default | Description |
|---|---|---|
| `BotShakedown.Enable` | `1` | Master switch. |
| `BotShakedown.Keywords` | `"cloth"` | Space-separated keywords that start a hand-over: any of `cloth`, `leather`, `ore`, `meat`, `herb`, `elemental`, `enchanting`. |

## License

MIT. See [LICENSE](LICENSE).
