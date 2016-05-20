/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RenameClasses.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "walkers.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "ReachableClasses.h"

#define MAX_DESCRIPTOR_LENGTH (1024)
#define MAX_IDENT_CHAR (52)
#define MAX_IDENT (MAX_IDENT_CHAR * MAX_IDENT_CHAR * MAX_IDENT_CHAR)

int match_short = 0;
int match_long = 0;
int match_inner = 0;

int base_strings_size = 0;
int ren_strings_size = 0;

static char getident(int num) {
  if (num < 26) {
    return 'a' + num;
  } else {
    return 'A' + num - 26;
  }
}

void get_next_ident(char *out, int &num) {
  // *sigh* re-write when not tired.
  int low = num;
  int mid = (num / 52);
  int top = (mid / 52);
  always_assert_log(num <= MAX_IDENT,
                    "Bailing, Ident %d, greater than maximum\n", num);
  if (top) {
    *out++ = getident(top - 1);
    low -= (top *52*52);
  }
  if (mid) {
    mid -= (top * 52);
    *out++ = getident(mid);
    low -= (mid * 52);
  }
  *out++ = getident(low);
  *out++ = '\0';
  num++;
}

void unpackage_private(Scope &scope) {
  walk_methods(scope,
	       [&](DexMethod *method) {
    if (is_package_protected(method)) set_public(method);
  });
  walk_fields(scope,
	       [&](DexField *field) {
    if (is_package_protected(field)) set_public(field);
  });
  for(auto clazz : scope) {
    if (is_package_protected(clazz)) set_public(clazz);
  }
}

bool should_rename(DexClass *clazz,
    std::vector<std::string>& pre_patterns,
    std::vector<std::string>& post_patterns) {
  auto chstring = clazz->get_type()->get_name()->c_str();
  /* We're assuming anonymous classes are safe always safe to rename */
  auto substr = strrchr(chstring, '$');
  if (substr != nullptr) {
    auto val = *++substr;
    if (val >= '0' && val <= '9') {
      match_inner++;
      return true;
    }
  }
  /* Check for more aggressive, but finer grained filters first */
  for (auto p : pre_patterns) {
    auto substr = strstr(chstring, p.c_str());
    if (substr != nullptr) {
      if (p.length() > 1) {
        match_long++;
      } else {
        match_short++;
      }
      return true;
    }
  }
  if (!can_rename(clazz)) {
    return false;
  }
  /* Check for wider, less precise filters */
  for (auto p : post_patterns) {
    auto substr = strstr(chstring, p.c_str());
    if (substr != nullptr) {
      if (p.length() > 1) {
        match_long++;
      } else {
        match_short++;
      }
      return true;
    }
  }
  return false;
}

void rename_classes(
  Scope& scope,
  std::vector<std::string>& pre_whitelist_patterns,
  std::vector<std::string>& post_whitelist_patterns,
  const std::string& path
) {
  unpackage_private(scope);
  int clazz_ident = 0;
  std::map<DexString*, DexString*> aliases;
  for(auto clazz: scope) {
    if (should_rename(clazz, pre_whitelist_patterns, post_whitelist_patterns)) {
      char clzname[4];
      char descriptor[10];
      get_next_ident(clzname, clazz_ident);
      // The X helps our hacked Dalvik classloader recognize that a
      // class name is the output of the redex renamer and thus will
      // never be found in the Android platform.
      sprintf(descriptor, "LX%s;", clzname);
      auto dstring = DexString::make_string(descriptor);
      auto dtype = clazz->get_type();
      auto oldname = dtype->get_name();
      aliases[oldname] = dstring;
      dtype->assign_name_alias(dstring);
      base_strings_size += strlen(oldname->c_str());
      base_strings_size += strlen(dstring->c_str());
      TRACE(RENAME, 4, "'%s'->'%s'\n", oldname->c_str(), descriptor);
      while(1) {
       std::string arrayop("[");
        arrayop += oldname->c_str();
        oldname = DexString::get_string(arrayop.c_str());
        if (oldname == nullptr) {
          break;
        }
        auto arraytype = DexType::get_type(oldname);
        if (arraytype == nullptr) {
          break;
        }
        std::string newarraytype("[");
        newarraytype += dstring->c_str();
        dstring = DexString::make_string(newarraytype.c_str());
        aliases[oldname] = dstring;
        arraytype->assign_name_alias(dstring);
      }
    }
  }
  /* Now we need to re-write the Signature annotations.  They use
   * Strings rather than Type's, so they have to be explicitly
   * handled.
   */

  /* Generics of the form Type<> turn into the Type string
   * sans the ';'.  So, we have to alias those if they
   * exist.  Signature annotations suck.
   */
  for (auto apair : aliases) {
    char buf[MAX_DESCRIPTOR_LENGTH];
    const char *sourcestr = apair.first->c_str();
    size_t sourcelen = strlen(sourcestr);
    if (sourcestr[sourcelen - 1] != ';')
      continue;
    strcpy(buf, sourcestr);
    buf[sourcelen - 1] = '\0';
    auto dstring = DexString::get_string(buf);
    if (dstring == nullptr) continue;
    strcpy(buf, apair.second->c_str());
    buf[strlen(apair.second->c_str()) - 1] = '\0';
    auto target = DexString::make_string(buf);
    aliases[dstring] = target;
  }
  walk_annotations(scope, [&](DexAnnotation* anno) {
    static DexType *dalviksig =
      DexType::get_type("Ldalvik/annotation/Signature;");
    if (anno->type() != dalviksig)
      return;
    auto elems = anno->anno_elems();
    for (auto elem : elems) {
      auto ev = elem.encoded_value;
      if (ev->evtype() != DEVT_ARRAY)
        continue;
      auto arrayev = static_cast<DexEncodedValueArray*>(ev);
      auto const& evs = arrayev->evalues();
      for (auto strev : *evs) {
        if (strev->evtype() != DEVT_STRING)
          continue;
        auto stringev = static_cast<DexEncodedValueString*>(strev);
        if (aliases.count(stringev->string())) {
          TRACE(RENAME, 5, "Rewriting Signature from '%s' to '%s'\n",
                stringev->string()->c_str(),
                aliases[stringev->string()]->c_str());
          stringev->string(aliases[stringev->string()]);
        }
      }
    }
  });

  if (!path.empty()) {
    FILE* fd = fopen(path.c_str(), "w");
    if (fd == nullptr) {
      perror("Error writing rename file");
      return;
    }
    for(const auto &it : aliases) {
      // record for later processing and back map generation
      fprintf(fd, "%s -> %s\n",it.first->c_str(),
      it.second->c_str());
    }
    fclose(fd);
  }

  for (auto clazz : scope) {
    clazz->get_vmethods().sort(compare_dexmethods);
    clazz->get_dmethods().sort(compare_dexmethods);
    clazz->get_sfields().sort(compare_dexfields);
    clazz->get_ifields().sort(compare_dexfields);
  }
}

void RenameClassesPass::run_pass(DexClassesVector& dexen, ConfigFiles& cfg) {
  auto scope = build_class_scope(dexen);
  rename_classes(scope, m_pre_filter_whitelist, m_post_filter_whitelist, m_path);
  TRACE(RENAME, 1, "renamed classes: %d anon classes, %d from single char patterns, %d from multi char patterns\n",
      match_inner,
      match_short,
      match_long);
  TRACE(RENAME, 1, "String savings, at least %d bytes \n", base_strings_size - ren_strings_size);
}
