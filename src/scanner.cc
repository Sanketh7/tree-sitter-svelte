#include "tag.hh"
#include "tree_sitter/parser.h"
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <iostream>
#include <string.h>
#include <string>
#include <vector>

#include "utils/ekstring.h"

namespace {

using std::cout;
using std::endl;
using std::string;
using std::vector;

enum TokenType {
  START_TAG_NAME,
  SCRIPT_START_TAG_NAME,
  STYLE_START_TAG_NAME,
  END_TAG_NAME,
  ERRONEOUS_END_TAG_NAME,
  SELF_CLOSING_TAG_DELIMITER,
  IMPLICIT_END_TAG,
  RAW_TEXT,
  RAW_TEXT_EXPR,
  RAW_TEXT_AWAIT,
  RAW_TEXT_EACH,
  COMMENT,
};

void copy_custom_tag(std::string str, char *buffer, unsigned int length) {
  strncpy(buffer, str.c_str(), length);
}

struct Scanner2 {
  vector<Tag> tags;

  Scanner2() {}

  unsigned serialize(char *buffer) {
    uint16_t tag_count = tags.size() > UINT16_MAX ? UINT16_MAX : tags.size();
    uint16_t serialized_tag_count = 0;

    unsigned i = sizeof(tag_count);
    std::memcpy(&buffer[i], &tag_count, sizeof(tag_count));
    i += sizeof(tag_count);

    for (; serialized_tag_count < tag_count; serialized_tag_count++) {
      Tag &tag = tags[serialized_tag_count];
      if (tag.type == CUSTOM) {
        unsigned name_length = tag.custom_tag_name.size();
        if (name_length > UINT8_MAX)
          name_length = UINT8_MAX;
        if (i + 2 + name_length >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE)
          break;
        buffer[i++] = static_cast<char>(tag.type);
        buffer[i++] = name_length;
        /* tag.custom_tag_name.copy(&buffer[i], name_length); */
        /* ekstring str = init_string_char(); */
        copy_custom_tag(tag.custom_tag_name, &buffer[i], name_length);
        i += name_length;
      } else {
        if (i + 1 >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE)
          break;
        buffer[i++] = static_cast<char>(tag.type);
      }
    }

    std::memcpy(&buffer[0], &serialized_tag_count,
                sizeof(serialized_tag_count));
    return i;
  }

  void custom_tag_name_assign(std::string &tag, const char *buf,
                              unsigned int length) {
    std::string str(buf, length);
    tag = str;
  }

  void deserialize(const char *buffer, unsigned length) {
    tags.clear();
    if (length > 0) {
      unsigned i = 0;
      uint16_t tag_count, serialized_tag_count;

      std::memcpy(&serialized_tag_count, &buffer[i],
                  sizeof(serialized_tag_count));
      i += sizeof(serialized_tag_count);

      std::memcpy(&tag_count, &buffer[i], sizeof(tag_count));
      i += sizeof(tag_count);

      tags.resize(tag_count);
      for (unsigned j = 0; j < serialized_tag_count; j++) {
        Tag &tag = tags[j];
        tag.type = static_cast<TagType>(buffer[i++]);
        if (tag.type == CUSTOM) {
          uint16_t name_length = static_cast<uint8_t>(buffer[i++]);
          tag.custom_tag_name.assign(&buffer[i], &buffer[i + name_length]);
          i += name_length;
        }
      }
    }
  }

  string scan_tag_name(TSLexer *lexer) {
    string tag_name;
    while (iswalnum(lexer->lookahead) || lexer->lookahead == '-' ||
           lexer->lookahead == ':') {
      tag_name += towupper(lexer->lookahead);
      lexer->advance(lexer, false);
    }
    return tag_name;
  }

  bool scan_comment(TSLexer *lexer) {
    if (lexer->lookahead != '-')
      return false;
    lexer->advance(lexer, false);
    if (lexer->lookahead != '-')
      return false;
    lexer->advance(lexer, false);

    unsigned dashes = 0;
    while (lexer->lookahead) {
      switch (lexer->lookahead) {
      case '-':
        ++dashes;
        break;
      case '>':
        if (dashes >= 2) {
          lexer->result_symbol = COMMENT;
          lexer->advance(lexer, false);
          lexer->mark_end(lexer);
          return true;
        }
      default:
        dashes = 0;
      }
      lexer->advance(lexer, false);
    }
    return false;
  }

  bool scan_raw_text(TSLexer *lexer) {
    if (!tags.size())
      return false;

    lexer->mark_end(lexer);

    const string &end_delimiter =
        tags.back().type == SCRIPT ? "</SCRIPT" : "</STYLE";

    unsigned delimiter_index = 0;

    while (lexer->lookahead) {
      if (towupper(lexer->lookahead) == end_delimiter[delimiter_index]) {
        delimiter_index++;
        if (delimiter_index == end_delimiter.size())
          break;
        lexer->advance(lexer, false);
      } else {
        delimiter_index = 0;
        lexer->advance(lexer, false);
        lexer->mark_end(lexer);
      }
    }

    lexer->result_symbol = RAW_TEXT;
    return true;
  }

  bool scan_implicit_end_tag(TSLexer *lexer) {
    Tag *parent = tags.empty() == 0 ? NULL : &tags.back();

    bool is_closing_tag = false;
    if (lexer->lookahead == '/') {
      is_closing_tag = true;
      lexer->advance(lexer, false);
    } else {
      if (parent && parent->is_void()) {
        tags.pop_back();
        lexer->result_symbol = IMPLICIT_END_TAG;
        return true;
      }
    }

    string tag_name = scan_tag_name(lexer);
    if (tag_name.size() == 0) {
      return false;
    }

    Tag next_tag = Tag::for_name(tag_name);

    if (is_closing_tag) {
      // The tag correctly closes the topmost element on the stack
      if (tags.size() != 0 && tags.back() == next_tag) {
        return false;
      }

      // Otherwise, dig deeper and queue implicit end tags (to be nice in
      // the case of malformed svelte)
      if (std::find(tags.begin(), tags.end(), next_tag) != tags.end()) {
        tags.pop_back();
        lexer->result_symbol = IMPLICIT_END_TAG;
        return true;
      }
    } else if (parent && !parent->can_contain(next_tag)) {
      tags.pop_back();
      lexer->result_symbol = IMPLICIT_END_TAG;
      return true;
    }

    return false;
  }

  bool scan_start_tag_name(TSLexer *lexer) {
    string tag_name = scan_tag_name(lexer);
    if (tag_name.empty())
      return false;
    Tag tag = Tag::for_name(tag_name);
    tags.push_back(tag);

    switch (tag.type) {
    case SCRIPT:
      lexer->result_symbol = SCRIPT_START_TAG_NAME;
      break;
    case STYLE:
      lexer->result_symbol = STYLE_START_TAG_NAME;
      break;
    default:
      lexer->result_symbol = START_TAG_NAME;
      break;
    }
    return true;
  }

  bool scan_end_tag_name(TSLexer *lexer) {
    string tag_name = scan_tag_name(lexer);
    if (tag_name.empty())
      return false;
    Tag tag = Tag::for_name(tag_name);
    if (!tags.empty() && tags.back() == tag) {
      tags.pop_back();
      lexer->result_symbol = END_TAG_NAME;
    } else {
      lexer->result_symbol = ERRONEOUS_END_TAG_NAME;
    }
    return true;
  }

  // NOTE: doesn't fire
  bool scan_self_closing_tag_delimiter(TSLexer *lexer) {
    lexer->advance(lexer, false);
    if (lexer->lookahead == '>') {
      lexer->advance(lexer, false);
      if (!tags.empty()) {
        tags.pop_back();
        lexer->result_symbol = SELF_CLOSING_TAG_DELIMITER;
      }
      return true;
    }
    return false;
  }

  bool scan_word(TSLexer *lexer, string word) {
    char c = lexer->lookahead;
    int i = 0;
    while (c == word[i++]) {
      lexer->advance(lexer, false);
      c = lexer->lookahead;
    }

    return (c == '{' || iswspace(c));
  }

  bool scan_raw_text_expr(TSLexer *lexer, TokenType extraToken) {
    char c = lexer->lookahead;
    int inner_curly_start = 0;

    while (c) {
      switch (c) {
      case '{': {
        inner_curly_start++;
        break;
      }
      case '}': {
        if (inner_curly_start <= 0) {
          lexer->mark_end(lexer);
          lexer->result_symbol = RAW_TEXT_EXPR;
          return true;
        }
        inner_curly_start--;
        break;
      }
      case '\n':
      case '\t':
      case ')':
      case ' ': {
        if (extraToken == RAW_TEXT_AWAIT || extraToken == RAW_TEXT_EACH) {
          lexer->mark_end(lexer);
          lexer->advance(lexer, false);
          c = lexer->lookahead;
          if (extraToken == RAW_TEXT_AWAIT && c == 't') {
            if (scan_word(lexer, "then")) {
              lexer->result_symbol = RAW_TEXT_AWAIT;
              return true;
            }
          } else if (extraToken == RAW_TEXT_EACH && c == 'a') {
            if (scan_word(lexer, "as")) {
              lexer->result_symbol = RAW_TEXT_EACH;
              return true;
            }
          }
        }
        break;
      }

      case '"':
      case '\'':
      case '`': {
        char quote = c;
        lexer->advance(lexer, false);
        c = lexer->lookahead;
        while (c) {
          if (c == 0)
            return false;
          if (c == '\\') {
            lexer->advance(lexer, false);
          }
          if (c == quote) {
            break;
          }
          lexer->advance(lexer, false);
          c = lexer->lookahead;
        }
        break;
      }
      default:;
      }
      lexer->advance(lexer, false);
      c = lexer->lookahead;
    }

    return false;
  }
  void printValidSymbols(const bool *valid_symbols) {
#define printValid(x)                                                          \
  if (valid_symbols[(x)])                                                      \
  printf("%s, ", (#x))
    printValid(START_TAG_NAME);
    printValid(SCRIPT_START_TAG_NAME);
    printValid(STYLE_START_TAG_NAME);
    printValid(END_TAG_NAME);
    printValid(ERRONEOUS_END_TAG_NAME);
    printValid(SELF_CLOSING_TAG_DELIMITER);
    printValid(IMPLICIT_END_TAG);
    printValid(RAW_TEXT);
    printValid(RAW_TEXT_EXPR);
    printValid(RAW_TEXT_AWAIT);
    printValid(RAW_TEXT_EACH);
    printValid(COMMENT);
#undef printValid
    printf("\n");
  }

  bool scan(TSLexer *lexer, const bool *valid_symbols) {
    while (iswspace(lexer->lookahead))
      lexer->advance(lexer, true);
    printf("scan->%c,", lexer->lookahead);
    printValidSymbols(valid_symbols);
    cout << tags.size() << endl;

    if (valid_symbols[RAW_TEXT_EXPR] && valid_symbols[RAW_TEXT_AWAIT]) {
      return scan_raw_text_expr(lexer, RAW_TEXT_AWAIT);
    }

    if (valid_symbols[RAW_TEXT_EXPR] && valid_symbols[RAW_TEXT_EACH]) {
      return scan_raw_text_expr(lexer, RAW_TEXT_EACH);
    }

    if (valid_symbols[RAW_TEXT_EXPR]) {
      char c = lexer->lookahead;
      if (c == '@' || c == '#' || c == ':' || c == '/')
        return false;
      return scan_raw_text_expr(lexer, RAW_TEXT_EXPR);
    }

    if (valid_symbols[RAW_TEXT] && !valid_symbols[START_TAG_NAME] &&
        !valid_symbols[END_TAG_NAME]) {
      return scan_raw_text(lexer);
    }

    switch (lexer->lookahead) {
    case '<':
      lexer->mark_end(lexer);
      lexer->advance(lexer, false);

      if (lexer->lookahead == '!') {
        lexer->advance(lexer, false);
        return scan_comment(lexer);
      }

      if (valid_symbols[IMPLICIT_END_TAG]) {
        return scan_implicit_end_tag(lexer);
      }
      break;

    case '\0':
      if (valid_symbols[IMPLICIT_END_TAG]) {
        bool b = scan_implicit_end_tag(lexer);
        return b;
      }
      break;

    case '/':
      if (valid_symbols[SELF_CLOSING_TAG_DELIMITER]) {
        return scan_self_closing_tag_delimiter(lexer);
      }
      break;

    default:
      if ((valid_symbols[START_TAG_NAME] || valid_symbols[END_TAG_NAME]) &&
          !valid_symbols[RAW_TEXT]) {
        return valid_symbols[START_TAG_NAME] ? scan_start_tag_name(lexer)
                                             : scan_end_tag_name(lexer);
      }
    }

    return false;
  }
};

extern "C" {

void *tree_sitter_svelte_external_scanner_create() { return new Scanner2(); }

bool tree_sitter_svelte_external_scanner_scan(void *payload, TSLexer *lexer,
                                              const bool *valid_symbols) {
  Scanner2 *scanner = static_cast<Scanner2 *>(payload);
  return scanner->scan(lexer, valid_symbols);
}

unsigned tree_sitter_svelte_external_scanner_serialize(void *payload,
                                                       char *buffer) {
  Scanner2 *scanner = static_cast<Scanner2 *>(payload);
  return scanner->serialize(buffer);
}

void tree_sitter_svelte_external_scanner_deserialize(void *payload,
                                                     const char *buffer,
                                                     unsigned length) {
  Scanner2 *scanner = static_cast<Scanner2 *>(payload);
  scanner->deserialize(buffer, length);
}

void tree_sitter_svelte_external_scanner_destroy(void *payload) {
  Scanner2 *scanner = static_cast<Scanner2 *>(payload);
  delete scanner;
}
} // extern "C"
} // namespace
