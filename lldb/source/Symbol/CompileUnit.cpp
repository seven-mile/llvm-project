//===-- CompileUnit.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Core/Module.h"
#include "lldb/Symbol/LineTable.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/Language.h"
#include "lldb/Utility/Timer.h"
#include <optional>

using namespace lldb;
using namespace lldb_private;

CompileUnit::CompileUnit(const lldb::ModuleSP &module_sp, void *user_data,
                         const char *pathname, const lldb::user_id_t cu_sym_id,
                         lldb::LanguageType language,
                         lldb_private::LazyBool is_optimized)
    : CompileUnit(module_sp, user_data,
                  std::make_shared<SupportFile>(FileSpec(pathname)), cu_sym_id,
                  language, is_optimized) {}

CompileUnit::CompileUnit(const lldb::ModuleSP &module_sp, void *user_data,
                         lldb::SupportFileSP support_file_sp,
                         const lldb::user_id_t cu_sym_id,
                         lldb::LanguageType language,
                         lldb_private::LazyBool is_optimized,
                         SupportFileList &&support_files)
    : ModuleChild(module_sp), UserID(cu_sym_id), m_user_data(user_data),
      m_language(language), m_flags(0),
      m_primary_support_file_sp(support_file_sp),
      m_support_files(std::move(support_files)), m_is_optimized(is_optimized) {
  if (language != eLanguageTypeUnknown)
    m_flags.Set(flagsParsedLanguage);
  assert(module_sp);
}

void CompileUnit::CalculateSymbolContext(SymbolContext *sc) {
  sc->comp_unit = this;
  GetModule()->CalculateSymbolContext(sc);
}

ModuleSP CompileUnit::CalculateSymbolContextModule() { return GetModule(); }

CompileUnit *CompileUnit::CalculateSymbolContextCompileUnit() { return this; }

void CompileUnit::DumpSymbolContext(Stream *s) {
  GetModule()->DumpSymbolContext(s);
  s->Printf(", CompileUnit{0x%8.8" PRIx64 "}", GetID());
}

void CompileUnit::GetDescription(Stream *s,
                                 lldb::DescriptionLevel level) const {
  const char *language = GetCachedLanguage();
  *s << "id = " << (const UserID &)*this << ", file = \""
     << this->GetPrimaryFile() << "\", language = \"" << language << '"';
}

void CompileUnit::ForeachFunction(
    llvm::function_ref<bool(const FunctionSP &)> lambda) const {
  std::vector<lldb::FunctionSP> sorted_functions;
  sorted_functions.reserve(m_functions_by_uid.size());
  for (auto &p : m_functions_by_uid)
    sorted_functions.push_back(p.second);
  llvm::sort(sorted_functions,
             [](const lldb::FunctionSP &a, const lldb::FunctionSP &b) {
               return a->GetID() < b->GetID();
             });

  for (auto &f : sorted_functions)
    if (lambda(f))
      return;
}

lldb::FunctionSP CompileUnit::FindFunction(
    llvm::function_ref<bool(const FunctionSP &)> matching_lambda) {
  LLDB_SCOPED_TIMER();

  lldb::ModuleSP module = CalculateSymbolContextModule();

  if (!module)
    return {};

  SymbolFile *symbol_file = module->GetSymbolFile();

  if (!symbol_file)
    return {};

  // m_functions_by_uid is filled in lazily but we need all the entries.
  symbol_file->ParseFunctions(*this);

  for (auto &p : m_functions_by_uid) {
    if (matching_lambda(p.second))
      return p.second;
  }
  return {};
}

const char *CompileUnit::GetCachedLanguage() const {
  if (m_flags.IsClear(flagsParsedLanguage))
    return "<not loaded>";
  return Language::GetNameForLanguageType(m_language);
}

// Dump the current contents of this object. No functions that cause on demand
// parsing of functions, globals, statics are called, so this is a good
// function to call to get an idea of the current contents of the CompileUnit
// object.
void CompileUnit::Dump(Stream *s, bool show_context) const {
  const char *language = GetCachedLanguage();

  s->Printf("%p: ", static_cast<const void *>(this));
  s->Indent();
  *s << "CompileUnit" << static_cast<const UserID &>(*this) << ", language = \""
     << language << "\", file = '" << GetPrimaryFile() << "'\n";

  //  m_types.Dump(s);

  if (m_variables.get()) {
    s->IndentMore();
    m_variables->Dump(s, show_context);
    s->IndentLess();
  }

  if (!m_functions_by_uid.empty()) {
    s->IndentMore();
    ForeachFunction([&s, show_context](const FunctionSP &f) {
      f->Dump(s, show_context);
      return false;
    });

    s->IndentLess();
    s->EOL();
  }
}

// Add a function to this compile unit
void CompileUnit::AddFunction(FunctionSP &funcSP) {
  m_functions_by_uid[funcSP->GetID()] = funcSP;
}

FunctionSP CompileUnit::FindFunctionByUID(lldb::user_id_t func_uid) {
  auto it = m_functions_by_uid.find(func_uid);
  if (it == m_functions_by_uid.end())
    return FunctionSP();
  return it->second;
}

lldb::LanguageType CompileUnit::GetLanguage() {
  if (m_language == eLanguageTypeUnknown) {
    if (m_flags.IsClear(flagsParsedLanguage)) {
      m_flags.Set(flagsParsedLanguage);
      if (SymbolFile *symfile = GetModule()->GetSymbolFile())
        m_language = symfile->ParseLanguage(*this);
    }
  }
  return m_language;
}

LineTable *CompileUnit::GetLineTable() {
  if (m_line_table_up == nullptr) {
    if (m_flags.IsClear(flagsParsedLineTable)) {
      m_flags.Set(flagsParsedLineTable);
      if (SymbolFile *symfile = GetModule()->GetSymbolFile())
        symfile->ParseLineTable(*this);
    }
  }
  return m_line_table_up.get();
}

void CompileUnit::SetLineTable(LineTable *line_table) {
  if (line_table == nullptr)
    m_flags.Clear(flagsParsedLineTable);
  else
    m_flags.Set(flagsParsedLineTable);
  m_line_table_up.reset(line_table);
}

DebugMacros *CompileUnit::GetDebugMacros() {
  if (m_debug_macros_sp.get() == nullptr) {
    if (m_flags.IsClear(flagsParsedDebugMacros)) {
      m_flags.Set(flagsParsedDebugMacros);
      if (SymbolFile *symfile = GetModule()->GetSymbolFile())
        symfile->ParseDebugMacros(*this);
    }
  }

  return m_debug_macros_sp.get();
}

void CompileUnit::SetDebugMacros(const DebugMacrosSP &debug_macros_sp) {
  if (debug_macros_sp.get() == nullptr)
    m_flags.Clear(flagsParsedDebugMacros);
  else
    m_flags.Set(flagsParsedDebugMacros);
  m_debug_macros_sp = debug_macros_sp;
}

VariableListSP CompileUnit::GetVariableList(bool can_create) {
  if (m_variables.get() == nullptr && can_create) {
    SymbolContext sc;
    CalculateSymbolContext(&sc);
    assert(sc.module_sp);
    sc.module_sp->GetSymbolFile()->ParseVariablesForContext(sc);
  }

  return m_variables;
}

std::vector<uint32_t>
FindFileIndexes(const SupportFileList &files, const FileSpec &file,
                RealpathPrefixes *realpath_prefixes = nullptr) {
  std::vector<uint32_t> result;
  uint32_t idx = -1;
  while ((idx = files.FindCompatibleIndex(idx + 1, file, realpath_prefixes)) !=
         UINT32_MAX)
    result.push_back(idx);
  return result;
}

uint32_t CompileUnit::FindLineEntry(uint32_t start_idx, uint32_t line,
                                    const FileSpec *file_spec_ptr, bool exact,
                                    LineEntry *line_entry_ptr) {
  if (!file_spec_ptr)
    file_spec_ptr = &GetPrimaryFile();
  std::vector<uint32_t> file_indexes = FindFileIndexes(GetSupportFiles(),
                                                       *file_spec_ptr);
  if (file_indexes.empty())
    return UINT32_MAX;

  // TODO: Handle SourceLocationSpec column information
  SourceLocationSpec location_spec(*file_spec_ptr, line,
                                   /*column=*/std::nullopt,
                                   /*check_inlines=*/false, exact);

  LineTable *line_table = GetLineTable();
  if (line_table)
    return line_table->FindLineEntryIndexByFileIndex(
        start_idx, file_indexes, location_spec, line_entry_ptr);
  return UINT32_MAX;
}

void CompileUnit::ResolveSymbolContext(
    const SourceLocationSpec &src_location_spec,
    SymbolContextItem resolve_scope, SymbolContextList &sc_list,
    RealpathPrefixes *realpath_prefixes) {
  const FileSpec file_spec = src_location_spec.GetFileSpec();
  const uint32_t line =
      src_location_spec.GetLine().value_or(LLDB_INVALID_LINE_NUMBER);
  const uint32_t column_num =
      src_location_spec.GetColumn().value_or(LLDB_INVALID_COLUMN_NUMBER);
  const bool check_inlines = src_location_spec.GetCheckInlines();

  // First find all of the file indexes that match our "file_spec". If
  // "file_spec" has an empty directory, then only compare the basenames when
  // finding file indexes
  bool file_spec_matches_cu_file_spec =
      FileSpec::Match(file_spec, this->GetPrimaryFile());

  // If we are not looking for inlined functions and our file spec doesn't
  // match then we are done...
  if (!file_spec_matches_cu_file_spec && !check_inlines)
    return;

  SymbolContext sc(GetModule());
  sc.comp_unit = this;

  if (line == LLDB_INVALID_LINE_NUMBER) {
    if (file_spec_matches_cu_file_spec && !check_inlines) {
      // only append the context if we aren't looking for inline call sites by
      // file and line and if the file spec matches that of the compile unit
      sc_list.Append(sc);
    }
    return;
  }

  std::vector<uint32_t> file_indexes =
      FindFileIndexes(GetSupportFiles(), file_spec, realpath_prefixes);
  const size_t num_file_indexes = file_indexes.size();
  if (num_file_indexes == 0)
    return;

  // Found a matching source file in this compile unit load its debug info.
  GetModule()->GetSymbolFile()->SetLoadDebugInfoEnabled();

  LineTable *line_table = sc.comp_unit->GetLineTable();

  if (line_table == nullptr) {
    if (file_spec_matches_cu_file_spec && !check_inlines) {
      sc_list.Append(sc);
    }
    return;
  }

  uint32_t line_idx;
  LineEntry line_entry;

  if (num_file_indexes == 1) {
    // We only have a single support file that matches, so use the line
    // table function that searches for a line entries that match a single
    // support file index
    line_idx = line_table->FindLineEntryIndexByFileIndex(
        0, file_indexes.front(), src_location_spec, &line_entry);
  } else {
    // We found multiple support files that match "file_spec" so use the
    // line table function that searches for a line entries that match a
    // multiple support file indexes.
    line_idx = line_table->FindLineEntryIndexByFileIndex(
        0, file_indexes, src_location_spec, &line_entry);
  }

  // If we didn't manage to find a breakpoint that matched the line number
  // requested, that might be because it is only an inline call site, and
  // doesn't have a line entry in the line table.  Scan for that here.
  //
  // We are making the assumption that if there was an inlined function it will
  // contribute at least 1 non-call-site entry to the line table.  That's handy
  // because we don't move line breakpoints over function boundaries, so if we
  // found a hit, and there were also a call site entry, it would have to be in
  // the function containing the PC of the line table match.  That way we can
  // limit the call site search to that function.
  // We will miss functions that ONLY exist as a call site entry.

  if (line_entry.IsValid() &&
      (line_entry.line != line ||
       (column_num != 0 && line_entry.column != column_num)) &&
      (resolve_scope & eSymbolContextLineEntry) && check_inlines) {
    // We don't move lines over function boundaries, so the address in the
    // line entry will be the in function that contained the line that might
    // be a CallSite, and we can just iterate over that function to find any
    // inline records, and dig up their call sites.
    Address start_addr = line_entry.range.GetBaseAddress();
    Function *function = start_addr.CalculateSymbolContextFunction();
    // Record the size of the list to see if we added to it:
    size_t old_sc_list_size = sc_list.GetSize();

    Declaration sought_decl(file_spec, line, column_num);
    // We use this recursive function to descend the block structure looking
    // for a block that has this Declaration as in it's CallSite info.
    // This function recursively scans the sibling blocks of the incoming
    // block parameter.
    std::function<void(Block &)> examine_block =
        [&sought_decl, &sc_list, &src_location_spec, resolve_scope,
         &examine_block](Block &block) -> void {
      // Iterate over the sibling child blocks of the incoming block.
      Block *sibling_block = block.GetFirstChild();
      while (sibling_block) {
        // We only have to descend through the regular blocks, looking for
        // immediate inlines, since those are the only ones that will have this
        // callsite.
        const InlineFunctionInfo *inline_info =
            sibling_block->GetInlinedFunctionInfo();
        if (inline_info) {
          // If this is the call-site we are looking for, record that:
          // We need to be careful because the call site from the debug info
          // will generally have a column, but the user might not have specified
          // it.
          Declaration found_decl = inline_info->GetCallSite();
          uint32_t sought_column = sought_decl.GetColumn();
          if (found_decl.FileAndLineEqual(sought_decl, false) &&
              (sought_column == LLDB_INVALID_COLUMN_NUMBER ||
               sought_column == found_decl.GetColumn())) {
            // If we found a call site, it belongs not in this inlined block,
            // but in the parent block that inlined it.
            Address parent_start_addr;
            if (sibling_block->GetParent()->GetStartAddress(
                    parent_start_addr)) {
              SymbolContext sc;
              parent_start_addr.CalculateSymbolContext(&sc, resolve_scope);
              // Now swap out the line entry for the one we found.
              LineEntry call_site_line = sc.line_entry;
              call_site_line.line = found_decl.GetLine();
              call_site_line.column = found_decl.GetColumn();
              bool matches_spec = true;
              // If the user asked for an exact match, we need to make sure the
              // call site we found actually matches the location.
              if (src_location_spec.GetExactMatch()) {
                matches_spec = false;
                if ((src_location_spec.GetFileSpec() ==
                     sc.line_entry.GetFile()) &&
                    (src_location_spec.GetLine() &&
                     *src_location_spec.GetLine() == call_site_line.line) &&
                    (src_location_spec.GetColumn() &&
                     *src_location_spec.GetColumn() == call_site_line.column))
                  matches_spec = true;
              }
              if (matches_spec &&
                  sibling_block->GetRangeAtIndex(0, call_site_line.range)) {
                SymbolContext call_site_sc(sc.target_sp, sc.module_sp,
                                           sc.comp_unit, sc.function, sc.block,
                                           &call_site_line, sc.symbol);
                sc_list.Append(call_site_sc);
              }
            }
          }
        }

        // Descend into the child blocks:
        examine_block(*sibling_block);
        // Now go to the next sibling:
        sibling_block = sibling_block->GetSibling();
      }
    };

    if (function) {
      // We don't need to examine the function block, it can't be inlined.
      Block &func_block = function->GetBlock(true);
      examine_block(func_block);
    }
    // If we found entries here, we are done.  We only get here because we
    // didn't find an exact line entry for this line & column, but if we found
    // an exact match from the call site info that's strictly better than
    // continuing to look for matches further on in the file.
    // FIXME: Should I also do this for "call site line exists between the
    // given line number and the later line we found in the line table"?  That's
    // a closer approximation to our general sliding algorithm.
    if (sc_list.GetSize() > old_sc_list_size)
      return;
  }

  // If "exact == true", then "found_line" will be the same as "line". If
  // "exact == false", the "found_line" will be the closest line entry
  // with a line number greater than "line" and we will use this for our
  // subsequent line exact matches below.
  const bool inlines = false;
  const bool exact = true;
  const std::optional<uint16_t> column =
      src_location_spec.GetColumn() ? std::optional<uint16_t>(line_entry.column)
                                    : std::nullopt;

  SourceLocationSpec found_entry(line_entry.GetFile(), line_entry.line, column,
                                 inlines, exact);

  while (line_idx != UINT32_MAX) {
    // If they only asked for the line entry, then we're done, we can
    // just copy that over. But if they wanted more than just the line
    // number, fill it in.
    SymbolContext resolved_sc;
    sc.line_entry = line_entry;
    if (resolve_scope == eSymbolContextLineEntry) {
      sc_list.Append(sc);
    } else {
      line_entry.range.GetBaseAddress().CalculateSymbolContext(&resolved_sc,
                                                               resolve_scope);
      // Sometimes debug info is bad and isn't able to resolve the line entry's
      // address back to the same compile unit and/or line entry. If the compile
      // unit changed, then revert back to just the compile unit and line entry.
      // Prior to this fix, the above code might end up not being able to lookup
      // the address, and then it would clear compile unit and the line entry in
      // the symbol context and the breakpoint would fail to get set even though
      // we have a valid line table entry in this compile unit. The address
      // lookup can also end up finding another function in another compiler
      // unit if the DWARF has overlappging address ranges. So if we end up with
      // no compile unit or a different one after the above function call,
      // revert back to the same results as if resolve_scope was set exactly to
      // eSymbolContextLineEntry.
      if (resolved_sc.comp_unit == this) {
        sc_list.Append(resolved_sc);
      } else {
        if (resolved_sc.comp_unit == nullptr && resolved_sc.module_sp) {
          // Only report an error if we don't map back to any compile unit. With
          // link time optimizations, the debug info might have many compile
          // units that have the same address range due to function outlining
          // or other link time optimizations. If the compile unit is NULL, then
          // address resolving is completely failing and more deserving of an
          // error message the user can see.
          resolved_sc.module_sp->ReportError(
              "unable to resolve a line table file address {0:x16} back "
              "to a compile unit, please file a bug and attach the address "
              "and file.",
              line_entry.range.GetBaseAddress().GetFileAddress());
        }
        sc_list.Append(sc);
      }
    }

    if (num_file_indexes == 1)
      line_idx = line_table->FindLineEntryIndexByFileIndex(
          line_idx + 1, file_indexes.front(), found_entry, &line_entry);
    else
      line_idx = line_table->FindLineEntryIndexByFileIndex(
          line_idx + 1, file_indexes, found_entry, &line_entry);
  }
}

bool CompileUnit::GetIsOptimized() {
  if (m_is_optimized == eLazyBoolCalculate) {
    m_is_optimized = eLazyBoolNo;
    if (SymbolFile *symfile = GetModule()->GetSymbolFile()) {
      if (symfile->ParseIsOptimized(*this))
        m_is_optimized = eLazyBoolYes;
    }
  }
  return m_is_optimized;
}

void CompileUnit::SetVariableList(VariableListSP &variables) {
  m_variables = variables;
}

const std::vector<SourceModule> &CompileUnit::GetImportedModules() {
  if (m_imported_modules.empty() &&
      m_flags.IsClear(flagsParsedImportedModules)) {
    m_flags.Set(flagsParsedImportedModules);
    if (SymbolFile *symfile = GetModule()->GetSymbolFile()) {
      SymbolContext sc;
      CalculateSymbolContext(&sc);
      symfile->ParseImportedModules(sc, m_imported_modules);
    }
  }
  return m_imported_modules;
}

bool CompileUnit::ForEachExternalModule(
    llvm::DenseSet<SymbolFile *> &visited_symbol_files,
    llvm::function_ref<bool(Module &)> lambda) {
  if (SymbolFile *symfile = GetModule()->GetSymbolFile())
    return symfile->ForEachExternalModule(*this, visited_symbol_files, lambda);
  return false;
}

const SupportFileList &CompileUnit::GetSupportFiles() {
  if (m_support_files.GetSize() == 0) {
    if (m_flags.IsClear(flagsParsedSupportFiles)) {
      m_flags.Set(flagsParsedSupportFiles);
      if (SymbolFile *symfile = GetModule()->GetSymbolFile())
        symfile->ParseSupportFiles(*this, m_support_files);
    }
  }
  return m_support_files;
}

void *CompileUnit::GetUserData() const { return m_user_data; }
