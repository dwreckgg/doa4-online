// The costume tables. See doa4_costumes.h.

#include "doa4_costumes.h"

#include <cstdio>

namespace doa4::costumes {

namespace {

uint16_t Be16(const GuestBytes& g, uint32_t a) { return uint16_t(g.Read(a) << 8 | g.Read(a + 1)); }

void PutBe16(const GuestBytes& g, uint32_t a, uint16_t v) {
  g.Write(a, uint8_t(v >> 8));
  g.Write(a + 1, uint8_t(v));
}

void PutBe32(const GuestBytes& g, uint32_t a, uint32_t v) {
  for (int i = 0; i < 4; ++i) g.Write(a + uint32_t(i), uint8_t(v >> (24 - 8 * i)));
}

bool InImage(uint32_t a) { return a >= kImageStart && a < kImageEnd; }

// Whether a list or variant pointer names a real table. Inside the XEX's heap (0x80000000-0x9FFFFFFF) only the image's own
// tables are (charId 22 and 23 hold a stray 0x831A649F there). Anywhere else it is a block one of the two systems moved
// her list to: an Extras box has already done so when the import is checked.
bool IsCostumeTable(uint32_t a) {
  const bool xex_heap = a >= 0x80000000u && a < 0xA0000000u;
  return a != 0 && (InImage(a) || !xex_heap);
}

std::string Hex(uint64_t v) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(v));
  return text;
}

std::string Pair(uint32_t a, uint32_t b) { return std::to_string(a) + "/" + std::to_string(b); }

}  // namespace

uint32_t RecordCount(const GuestBytes& g, uint32_t c) {
  if (c >= kCharCount) return 0;
  const uint32_t start = g.ReadBe32(kModelTable + 8 * c + 4);
  if (!InImage(start)) return 0;
  uint32_t next = 0xFFFFFFFFu;
  for (uint32_t k = 0; k < kCharCount; ++k) {
    const uint32_t p = g.ReadBe32(kModelTable + 8 * k + 4);
    if (InImage(p) && p > start && p < next) next = p;
  }
  return next == 0xFFFFFFFFu ? 0 : (next - start) / 8;
}

GuardState ReadGuardState(const GuestBytes& g) {
  GuardState s;
  for (uint32_t c = 0; c < kCharCount; ++c) s.records[c] = RecordCount(g, c);
  return s;
}

std::optional<FileLoc> FileLocation(const GuestBytes& g, uint32_t file) {
  if (file >= kFileCount) return std::nullopt;
  const uint32_t at = kFileTable + 4 * file;
  const uint32_t entry = Be16(g, at + 2);
  switch (g.Read(at)) {
    case 3:
      return FileLoc{"chrone.afs", entry};
    case 4:
      return FileLoc{"chrtwo.afs", entry};
    case 5:
      return FileLoc{"chrthree.afs", entry};
    case 0:
      return FileLoc{"avt.afs", entry};
    default:
      return std::nullopt;
  }
}

namespace {
// Only the three character archives hold a suit's files (FileLocation also names avt.afs, for the colour costumes).
bool CharacterFile(const GuestBytes& g, uint32_t file) {
  const std::optional<FileLoc> loc = FileLocation(g, file);
  return loc && std::string(loc->archive) != "avt.afs";
}
}  // namespace

std::string CheckSuit(const GuestBytes& g, const suits::Suit& s) {
  const uint32_t c = s.character;
  if (c >= kCharCount) return "no such character";
  if (!IsCostumeTable(g.ReadBe32(kListTable + 4 * c)) || !IsCostumeTable(g.ReadBe32(kVarTable + 4 * c))) {
    return "this character has no costume list in the game data";
  }
  if (!CharacterFile(g, s.body_tpr) || !CharacterFile(g, s.body_cat)) {
    return "files " + Pair(s.body_tpr, s.body_cat) + " are not character archive files";
  }
  const uint32_t split_tpr = g.ReadBe32(kSplitTable + 8 * c);
  const uint32_t split_cat = g.ReadBe32(kSplitTable + 8 * c + 4);
  const bool whole = split_tpr == 0 && split_cat == 0;
  const bool has_part = s.part_tpr != suits::kNoPart;
  if (whole && has_part) return "she has no head part, but the suit names one";
  if (!whole && !has_part) return "a head part is needed (her budget is split with one)";
  const uint64_t max_tpr = whole ? kWholeTpr : split_tpr;
  const uint64_t max_cat = whole ? kWholeCat : split_cat;
  if (s.tpr.size > max_tpr) {
    return "the model file is " + Hex(s.tpr.size) + " bytes, over her budget of " + Hex(max_tpr);
  }
  if (s.cat.size > max_cat) {
    return "the .cat file is " + Hex(s.cat.size) + " bytes, over her budget of " + Hex(max_cat);
  }
  if (s.own_model >= 0) {
    const uint32_t m = uint32_t(s.own_model);
    const uint32_t count = RecordCount(g, c);
    if (m >= count) {
      return "model record m" + std::to_string(m) + " does not exist (she has " +
             std::to_string(count) + ")";
    }
    const uint32_t r = g.ReadBe32(kModelTable + 8 * c + 4) + 8 * m;
    if (Be16(g, r) != s.body_tpr || Be16(g, r + 2) != s.body_cat) {
      return "model record m" + std::to_string(m) + " holds files " + Pair(Be16(g, r), Be16(g, r + 2)) +
             ", not the suit's " + Pair(s.body_tpr, s.body_cat);
    }
    const uint16_t pt = Be16(g, r + 4), pc = Be16(g, r + 6);
    const bool empty = pt == 0xFFFF && pc == 0xFFFF;
    if (!empty && !(pt == s.part_tpr && pc == s.part_cat)) {
      return "model record m" + std::to_string(m) + " already has head part " + Pair(pt, pc);
    }
  }
  if (has_part) {
    // The pair must be one a record of hers already uses (an empty FFFF/FFFF record never equals a
    // named part), and both files must be character archive files: a hand-edited manifest could
    // otherwise load another girl's head, or a motion file, as her head at fight time.
    if (!CharacterFile(g, s.part_tpr) || !CharacterFile(g, s.part_cat)) {
      return "head part files " + Pair(s.part_tpr, s.part_cat) + " are not character archive files";
    }
    const uint32_t records = g.ReadBe32(kModelTable + 8 * c + 4);
    const uint32_t count = RecordCount(g, c);
    for (uint32_t m = 0; m < count; ++m) {
      const uint32_t r = records + 8 * m;
      if (Be16(g, r + 4) == s.part_tpr && Be16(g, r + 6) == s.part_cat) return {};
    }
    return "head part " + Pair(s.part_tpr, s.part_cat) + " is not one of her head parts";
  }
  return {};
}

std::vector<suits::Suit> Fitting(const GuestBytes& g, const std::vector<suits::Suit>& suits, std::vector<std::string>* refused) {
  std::vector<suits::Suit> out;
  for (const suits::Suit& s : suits) {
    const std::string why = CheckSuit(g, s);
    if (why.empty()) {
      out.push_back(s);
    } else if (refused) {
      refused->push_back(s.Label() + ": " + why);
    }
  }
  return out;
}

ApplyResult Apply(const GuestBytes& g, const AllocFn& alloc, const std::vector<suits::Suit>& suits) {
  return Apply(g, alloc, std::vector<Extra>{}, suits);
}

ApplyResult Apply(const GuestBytes& g, const AllocFn& alloc, const std::vector<Extra>& extras,
                  const std::vector<suits::Suit>& suits) {
  ApplyResult result;
  result.guard = ReadGuardState(g);
  // One accepted costume of hers: an extra or a suit, in the order they go in (her extras, then her suits).
  struct Item {
    const Extra* extra = nullptr;
    const suits::Suit* suit = nullptr;
    uint32_t list_index = 0;  // a replacing extra: where its model goes in her list
  };
  for (uint32_t c = 0; c < kCharCount; ++c) {
    const std::string who = "charId " + std::to_string(c);
    const uint32_t count = result.guard.records[c];
    const uint32_t records = g.ReadBe32(kModelTable + 8 * c + 4);
    const costume_lists::Tables now = costume_lists::Read(g, c);  // as they stand: after the Extras boxes
    const uint32_t len = now.length;
    const uint32_t max = now.maximum;
    // Her extras and then her suits in order, each while it still fits: one past a limit is refused by name and the ones
    // before it stay. The limits count extras and suits together. Model index 255 means "none" to the loader
    // (sub_823FE870 returns without loading, .9.cpp:53983-53987), so the highest index an append may produce is 254:
    // count + appends <= 255. Every extra appends a record; a replacing one adds no costume.
    std::vector<Item> mine;
    uint32_t appends = 0;  // records appended
    uint32_t added = 0;    // costumes appended (list entries with one variant each)
    std::vector<bool> replaced(256, false);
    bool any = false;
    const auto refuse = [&](const std::string& label, const std::string& why) {
      result.errors.push_back(who + ": " + label + ": " + why);
      result.refused[label] = why;
    };
    const std::string too_many = "too many costumes (at most " + std::to_string(kMaxCostumesLimit) + "); not added";
    for (const Extra& e : extras) {
      if (e.character != c) continue;
      any = true;
      if (e.replaces == -1) {
        const uint32_t taken = added + 1;
        if (len + taken > kTableBlock - 1 || max + taken > kMaxCostumesLimit || count + appends + 1 > 255) {
          refuse(e.label, too_many);
          continue;
        }
        mine.push_back({&e, nullptr, 0});
        ++added;
        ++appends;
        continue;
      }
      // A replacing extra: her costume `replaces` (as her tables stand, before this pass) with one variant becomes the
      // new model at list index sum(var[0..replaces)), the lookup's index for its variant 0 (sub_82526B78).
      if (e.replaces < 0 || uint32_t(e.replaces) >= max) {
        refuse(e.label, "costume " + std::to_string(e.replaces) + " does not exist (she has " + std::to_string(max) +
                            "); not added");
        continue;
      }
      const uint32_t r = uint32_t(e.replaces);
      const uint32_t variants = g.Read(now.variants + r);
      if (variants != 1) {
        refuse(e.label, "costume " + std::to_string(r) + " has " + std::to_string(variants) +
                            " variants (only a costume with one can be replaced); not added");
        continue;
      }
      uint32_t index = 0;
      for (uint32_t k = 0; k < r; ++k) index += g.Read(now.variants + k);
      if (index >= len) {
        refuse(e.label, "costume " + std::to_string(r) + " is at list index " + std::to_string(index) +
                            ", past her list of " + std::to_string(len) + "; not added");
        continue;
      }
      if (replaced[r]) {
        refuse(e.label, "costume " + std::to_string(r) + " is already replaced by another extra; not added");
        continue;
      }
      if (count + appends + 1 > 255) {
        refuse(e.label, "too many model records (at most 255); not added");
        continue;
      }
      replaced[r] = true;
      mine.push_back({&e, nullptr, index});
      ++appends;
    }
    for (const suits::Suit& s : suits) {
      if (s.character != c) continue;
      any = true;
      const uint32_t taken = added + 1;
      const uint32_t would_append = appends + (s.own_model < 0 ? 1u : 0u);
      if (len + taken > kTableBlock - 1 || max + taken > kMaxCostumesLimit || count + would_append > 255) {
        refuse(s.Label(), too_many);
        continue;
      }
      mine.push_back({nullptr, &s, 0});
      ++added;
      appends = would_append;
    }
    if (!any || mine.empty()) continue;
    if (appends && count == 0) {
      result.errors.push_back(who + ": her model records cannot be counted; " +
                              std::string(mine.front().extra ? "colour costumes and suits" : "suits") + " not added");
      continue;
    }
    const uint32_t record_block = appends ? alloc(8 * (count + appends)) : 0;
    const uint32_t list_block = (!appends || record_block) ? alloc(kTableBlock) : 0;
    const uint32_t var_block = list_block ? alloc(kTableBlock) : 0;
    if ((appends && !record_block) || !list_block || !var_block) {
      result.errors.push_back(who + ": no guest memory for her tables; suits not added");
      continue;
    }
    uint32_t current = records;
    if (appends) {
      for (uint32_t i = 0; i < 8 * count; ++i) g.Write(record_block + i, g.Read(records + i));
      current = record_block;
    }
    uint32_t next_model = count;
    std::vector<uint8_t> appended;                                // models of the appended costumes, in order
    std::vector<std::pair<uint32_t, uint32_t>> replacements;      // (list index, model)
    for (const Item& item : mine) {
      uint32_t m = 0;
      if (item.extra) {
        m = next_model++;
        const uint32_t r = current + 8 * m;
        PutBe16(g, r, item.extra->body_tpr);
        PutBe16(g, r + 2, item.extra->body_cat);
        PutBe16(g, r + 4, 0xFFFF);
        PutBe16(g, r + 6, 0xFFFF);
        const bool replacing = item.extra->replaces != -1;
        if (replacing) replacements.push_back({item.list_index, m});
        if (!replacing) appended.push_back(uint8_t(m));
        result.applied.push_back({uint8_t(c), m, replacing ? uint32_t(item.extra->replaces) : max + uint32_t(appended.size()) - 1,
                                  item.extra->label, replacing});
        continue;
      }
      const suits::Suit* s = item.suit;
      if (s->own_model >= 0) {
        m = uint32_t(s->own_model);
        const uint32_t r = current + 8 * m;
        if (s->part_tpr != suits::kNoPart && Be16(g, r + 4) == 0xFFFF && Be16(g, r + 6) == 0xFFFF) {
          PutBe16(g, r + 4, s->part_tpr);
          PutBe16(g, r + 6, s->part_cat);
        }
      } else {
        m = next_model++;
        const uint32_t r = current + 8 * m;
        PutBe16(g, r, s->body_tpr);
        PutBe16(g, r + 2, s->body_cat);
        PutBe16(g, r + 4, s->part_tpr);
        PutBe16(g, r + 6, s->part_cat);
      }
      appended.push_back(uint8_t(m));
      result.applied.push_back({uint8_t(c), m, max + uint32_t(appended.size()) - 1, s->Label()});
    }
    costume_lists::FillMoved(g, now, appended, list_block, var_block, kTableBlock);
    for (const auto& [index, m] : replacements) g.Write(list_block + index, uint8_t(m));
    if (appends) PutBe32(g, kModelTable + 8 * c + 4, record_block);
    PutBe32(g, kListTable + 4 * c, list_block);
    PutBe32(g, kVarTable + 4 * c, var_block);
    g.Write(kLenTable + c, uint8_t(len + added));
    g.Write(kMaxTable + c, uint8_t(max + added));
    result.guard.records[c] = count + appends;
  }
  return result;
}

std::vector<uint8_t> TableBytes(const GuestBytes& g, const GuardState& state) {
  std::vector<uint8_t> out;
  const auto put32 = [&](uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i)));
  };
  for (uint32_t c = 0; c < kCharCount; ++c) {
    const uint32_t max = g.Read(kMaxTable + c), len = g.Read(kLenTable + c);
    const uint32_t list = g.ReadBe32(kListTable + 4 * c), var = g.ReadBe32(kVarTable + 4 * c);
    const uint32_t records = g.ReadBe32(kModelTable + 8 * c + 4);
    out.push_back(uint8_t(max));
    out.push_back(uint8_t(len));
    put32(list);
    put32(var);
    put32(records);
    if (IsCostumeTable(list)) {
      for (uint32_t i = 0; i < len; ++i) out.push_back(g.Read(list + i));
    }
    if (IsCostumeTable(var)) {
      for (uint32_t i = 0; i < max; ++i) out.push_back(g.Read(var + i));
    }
    if (records != 0) {
      for (uint32_t i = 0; i < 8 * state.records[c]; ++i) out.push_back(g.Read(records + i));
    }
  }
  return out;
}

uint8_t VisibleCount(uint8_t save_count, uint8_t list_length, bool has_suits) {
  return has_suits && list_length > save_count ? list_length : save_count;
}

SuitCounts CountSuits(const std::vector<Applied>& applied) {
  SuitCounts counts{};
  for (const Applied& a : applied) {
    if (!a.replaced && a.character < kCharCount && counts[a.character] < 255) ++counts[a.character];
  }
  return counts;
}

std::vector<Raised> RaiseCountRow(const GuestBytes& g, uint32_t row, const SuitCounts& suits) {
  std::vector<Raised> changed;
  for (uint32_t c = 0; c < kCharCount; ++c) {
    if (suits[c] == 0) continue;
    const uint8_t save = g.Read(row + c);
    if (save == 0) continue;  // the game did not fill this one (a save's count is 2 or more): not made up here
    const uint8_t of = g.Read(kMaxTable + c);
    const uint8_t shows = VisibleCount(save, of, true);
    if (shows == save) continue;
    g.Write(row + c, shows);
    changed.push_back({uint8_t(c), save, shows, of});
  }
  return changed;
}

GuardRule RuleFor(const GuardState& state, uint32_t character, uint32_t model) {
  if (model == 255) return GuardRule::kNone;  // 255 = "none" to the loader (sub_823FE870)
  if (character >= kCharCount) return GuardRule::kNone;
  const uint32_t count = state.records[character];
  if (count != 0 && model >= count) return GuardRule::kPastRecords;
  if (state.empty_slot[character].contains(model)) return GuardRule::kEmptySlot;
  return GuardRule::kNone;
}

uint32_t GuardModel(const GuardState& state, uint32_t character, uint32_t model, uint32_t first_listed) {
  return RuleFor(state, character, model) == GuardRule::kNone ? model : first_listed;
}

LoadDecision DecideLoad(const GuardState* state, const GuestBytes& guest, uint32_t r4, uint32_t r5) {
  LoadDecision d;
  d.character = r4 & 0xFF;
  d.asked = r5 & 0xFF;
  d.use = d.asked;
  if (!state) return d;
  d.rule = RuleFor(*state, d.character, d.asked);
  if (d.rule == GuardRule::kNone) return d;
  const uint32_t list = guest.ReadBe32(kListTable + 4 * d.character);
  d.use = GuardModel(*state, d.character, d.asked, guest.Read(list));
  return d;
}

uint32_t RequestModel(const LoadDecision& d, bool mixed) { return mixed && d.rule == GuardRule::kEmptySlot ? d.asked : d.use; }

LoaderTick BeginLoaderTick(const GuardState* state, const GuestBytes& guest, uint32_t slot, bool mixed) {
  LoaderTick tick;
  if (!mixed || !state || slot >= kLoadSlotCount) return tick;
  const uint32_t row = kLoadSlots + slot * kLoadSlotBytes;
  if (guest.Read(row) == 255) return tick;  // idle: sub_823FE150 returns at once
  const LoadDecision d = DecideLoad(state, guest, guest.Read(row + 3), guest.Read(row + 4));
  if (d.rule != GuardRule::kEmptySlot) return tick;
  tick.swapped = true;
  tick.character = uint8_t(d.character);
  tick.asked = uint8_t(d.asked);
  tick.file = uint8_t(d.use);
  guest.Write(row + 4, tick.file);
  return tick;
}

void EndLoaderTick(const LoaderTick& tick, const GuestBytes& guest, uint32_t slot) {
  if (!tick.swapped) return;
  const uint32_t row = kLoadSlots + slot * kLoadSlotBytes;
  if (guest.Read(row + 3) == tick.character && guest.Read(row + 4) == tick.file) guest.Write(row + 4, tick.asked);
  if (guest.Read(row + 5) == tick.character && guest.Read(row + 6) == tick.file) guest.Write(row + 6, tick.asked);
}

}  // namespace doa4::costumes
