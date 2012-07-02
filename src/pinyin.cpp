/* 
 *  libpinyin
 *  Library to deal with pinyin.
 *  
 *  Copyright (C) 2011 Peng Wu <alexepico@gmail.com>
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */


#include "pinyin.h"
#include <stdio.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include "pinyin_internal.h"

/* a glue layer for input method integration. */

struct _pinyin_context_t{
    pinyin_option_t m_options;

    FullPinyinParser2 * m_full_pinyin_parser;
    DoublePinyinParser2 * m_double_pinyin_parser;
    ChewingParser2 * m_chewing_parser;

    FacadeChewingTable * m_pinyin_table;
    FacadePhraseTable * m_phrase_table;
    FacadePhraseIndex * m_phrase_index;
    Bigram * m_system_bigram;
    Bigram * m_user_bigram;

    PinyinLookup * m_pinyin_lookup;
    PhraseLookup * m_phrase_lookup;

    char * m_system_dir;
    char * m_user_dir;
    bool m_modified;
};

struct _import_iterator_t{
    pinyin_context_t * m_context;
    guint8 m_phrase_index;
};

static bool check_format(const char * userdir){
    gchar * filename = g_build_filename
        (userdir, "version", NULL);

    MemoryChunk chunk;
    bool exists = chunk.load(filename);

    if (exists) {
        exists = (0 == memcmp
                  (LIBPINYIN_FORMAT_VERSION, chunk.begin(),
                   strlen(LIBPINYIN_FORMAT_VERSION) + 1));
    }
    g_free(filename);

    if (exists)
        return exists;

    /* clean up files, if version mis-matches. */
    for (size_t i = 1; i < PHRASE_INDEX_LIBRARY_COUNT; ++i) {
        const pinyin_table_info_t * table_info = pinyin_phrase_files + i;

        if (NOT_USED == table_info->m_file_type)
            continue;

        if (NULL == table_info->m_user_filename)
            continue;

        const char * userfilename = table_info->m_user_filename;

        /* remove dbin file. */
        filename = g_build_filename(userdir, userfilename, NULL);
        unlink(filename);
        g_free(filename);
    }

    filename = g_build_filename
        (userdir, "user.db", NULL);
    unlink(filename);
    g_free(filename);

    return exists;
}

static bool mark_version(const char * userdir){
    gchar * filename = g_build_filename
        (userdir, "version", NULL);
    MemoryChunk chunk;
    chunk.set_content(0, LIBPINYIN_FORMAT_VERSION,
                      strlen(LIBPINYIN_FORMAT_VERSION) + 1);
    bool retval = chunk.save(filename);
    g_free(filename);
    return retval;
}

pinyin_context_t * pinyin_init(const char * systemdir, const char * userdir){
    pinyin_context_t * context = new pinyin_context_t;

    context->m_options = USE_TONE;

    context->m_system_dir = g_strdup(systemdir);
    context->m_user_dir = g_strdup(userdir);
    context->m_modified = false;

    check_format(context->m_user_dir);

    context->m_full_pinyin_parser = new FullPinyinParser2;
    context->m_double_pinyin_parser = new DoublePinyinParser2;
    context->m_chewing_parser = new ChewingParser2;

    /* load chewing table. */
    context->m_pinyin_table = new FacadeChewingTable;

    /* load system chewing table. */
    MemoryChunk * chunk = new MemoryChunk;
    gchar * filename = g_build_filename
        (context->m_system_dir, "pinyin_index.bin", NULL);
    if (!chunk->load(filename)) {
        fprintf(stderr, "open %s failed!\n", filename);
        return NULL;
    }
    g_free(filename);

    /* load user chewing table */
    MemoryChunk * userchunk = new MemoryChunk;
    filename = g_build_filename
        (context->m_user_dir, "user_pinyin_index.bin", NULL);
    if (!userchunk->load(filename)) {
        /* hack here: use local Chewing Table to create empty memory chunk. */
        ChewingLargeTable table(context->m_options);
        table.store(userchunk);
    }
    g_free(filename);

    context->m_pinyin_table->load(context->m_options, chunk, userchunk);

    /* load phrase table */
    context->m_phrase_table = new FacadePhraseTable;

    /* load system phrase table */
    chunk = new MemoryChunk;
    filename = g_build_filename
        (context->m_system_dir, "phrase_index.bin", NULL);
    if (!chunk->load(filename)) {
        fprintf(stderr, "open %s failed!\n", filename);
        return NULL;
    }
    g_free(filename);

    /* load user phrase table */
    userchunk = new MemoryChunk;
    filename = g_build_filename
        (context->m_user_dir, "user_phrase_index.bin", NULL);
    if (!userchunk->load(filename)) {
        /* hack here: use local Phrase Table to create empty memory chunk. */
        PhraseLargeTable table;
        table.store(userchunk);
    }
    g_free(filename);

    context->m_phrase_table->load(chunk, userchunk);

    context->m_phrase_index = new FacadePhraseIndex;

    /* hack here: directly call load phrase library. */
    pinyin_load_phrase_library(context, 1);

    context->m_system_bigram = new Bigram;
    filename = g_build_filename(context->m_system_dir, "bigram.db", NULL);
    context->m_system_bigram->attach(filename, ATTACH_READONLY);
    g_free(filename);

    context->m_user_bigram = new Bigram;
    filename = g_build_filename(context->m_user_dir, "user.db", NULL);
    context->m_user_bigram->load_db(filename);
    g_free(filename);

    context->m_pinyin_lookup = new PinyinLookup
        ( context->m_options, context->m_pinyin_table,
          context->m_phrase_index, context->m_system_bigram,
          context->m_user_bigram);

    context->m_phrase_lookup = new PhraseLookup
        (context->m_phrase_table, context->m_phrase_index,
         context->m_system_bigram, context->m_user_bigram);

    return context;
}

bool pinyin_load_phrase_library(pinyin_context_t * context,
                                guint8 index){
    assert(index < PHRASE_INDEX_LIBRARY_COUNT);
    const pinyin_table_info_t * table_info = pinyin_phrase_files + index;

    if (SYSTEM_FILE == table_info->m_file_type) {
        /* system phrase library */
        MemoryChunk * chunk = new MemoryChunk;

        const char * systemfilename = table_info->m_system_filename;
        /* check bin file in system dir. */
        gchar * chunkfilename = g_build_filename(context->m_system_dir,
                                                 systemfilename, NULL);
        chunk->load(chunkfilename);
        g_free(chunkfilename);

        context->m_phrase_index->load(index, chunk);

        const char * userfilename = table_info->m_user_filename;

        chunkfilename = g_build_filename(context->m_user_dir,
                                         userfilename, NULL);

        MemoryChunk * log = new MemoryChunk;
        log->load(chunkfilename);
        g_free(chunkfilename);

        /* merge the chunk log. */
        context->m_phrase_index->merge(index, log);
        return true;
    }

    if (USER_FILE == table_info->m_file_type) {
        /* user phrase library */
        MemoryChunk * chunk = new MemoryChunk;
        const char * userfilename = table_info->m_user_filename;

        gchar * chunkfilename = g_build_filename(context->m_user_dir,
                                                 userfilename, NULL);

	/* check bin file exists. if not, create a new one. */
        if (chunk->load(chunkfilename)) {
            context->m_phrase_index->load(index, chunk);
        } else {
            delete chunk;
            context->m_phrase_index->create_sub_phrase(index);
        }

        g_free(chunkfilename);
        return true;
    }

    return false;
}

bool pinyin_unload_phrase_library(pinyin_context_t * context,
                                  guint8 index){
    /* gb_char.bin can't be unloaded. */
    if (1 == index)
        return false;

    assert(index < PHRASE_INDEX_LIBRARY_COUNT);

    context->m_phrase_index->unload(index);
    return true;
}

import_iterator_t * pinyin_begin_add_phrases(pinyin_context_t * context,
                                             guint8 index){
    import_iterator_t * iter = new import_iterator_t;
    iter->m_context = context;
    iter->m_phrase_index = index;
    return iter;
}

bool pinyin_iterator_add_phrase(import_iterator_t * iter,
                                const char * phrase,
                                const char * pinyin,
                                gint count){
    /* if -1 == count, use the default value. */
    const int default_count = 100;
    if (-1 == count)
        count = default_count;

    pinyin_context_t * & context = iter->m_context;
    FacadePhraseTable * & phrase_table = context->m_phrase_table;
    FacadeChewingTable * & pinyin_table = context->m_pinyin_table;
    FacadePhraseIndex * & phrase_index = context->m_phrase_index;

    /* check whether the phrase exists in phrase table */
    glong len_phrase = 0;
    ucs4_t * ucs4_phrase = g_utf8_to_ucs4(phrase, -1, NULL, &len_phrase, NULL);
    phrase_token_t token = null_token;

    bool result = false;

    pinyin_option_t options = PINYIN_CORRECT_ALL | USE_TONE;
    FullPinyinParser2 parser;
    ChewingKeyVector keys =
        g_array_new(FALSE, FALSE, sizeof(ChewingKey));
    ChewingKeyRestVector key_rests =
        g_array_new(FALSE, FALSE, sizeof(ChewingKeyRest));

    PhraseItem item;
    int retval = phrase_table->search(len_phrase, ucs4_phrase, token);
    if (!(retval & SEARCH_OK)) {
        /* if not exists, get the maximum token,
           then add it directly with maximum token + 1; */
        PhraseIndexRange range;
        retval = phrase_index->get_range(iter->m_phrase_index, range);

        if (ERROR_OK == retval) {
            token = range.m_range_end;

            /* parse the pinyin. */
            parser.parse(options, keys, key_rests, pinyin, strlen(pinyin));

            if ( len_phrase == keys->len ) { /* valid pinyin */
                phrase_table->add_index(len_phrase, ucs4_phrase, token);
                pinyin_table->add_index
                    (keys->len, (ChewingKey *)(keys->data), token);

                item.set_phrase_string(len_phrase, ucs4_phrase);
                item.append_pronunciation((ChewingKey *)(keys->data), count);
                phrase_index->add_phrase_item(token, &item);
                result = true;
            }
        }
    } else {
        /* if exists, check whether in the same sub phrase; */
        if (PHRASE_INDEX_LIBRARY_INDEX(token) == iter->m_phrase_index) {
            /* if so, remove the phrase, add the pinyin for the phrase item,
               then add it back;*/
            phrase_index->get_phrase_item(token, item);
            assert(len_phrase == item.get_phrase_length());
            ucs4_t tmp_phrase[MAX_PHRASE_LENGTH];
            item.get_phrase_string(tmp_phrase);
            assert(0 == memcmp
                   (ucs4_phrase, tmp_phrase, sizeof(ucs4_t) * len_phrase));

            /* parse the pinyin. */
            parser.parse(options, keys, key_rests, pinyin, strlen(pinyin));

            PhraseItem * removed_item = NULL;
            retval = phrase_index->remove_phrase_item(token, removed_item);
            if (ERROR_OK == retval) {
                removed_item->append_pronunciation((ChewingKey *)keys->data,
                                                   count);
                phrase_index->add_phrase_item(token, removed_item);
                delete removed_item;
            }
        } else {
            /* if not, return false; */
        }
    }

    g_array_free(key_rests, TRUE);
    g_array_free(keys, TRUE);
    g_free(ucs4_phrase);
    return result;
}

void pinyin_end_add_phrases(import_iterator_t * iter){
    /* compact the content memory chunk of phrase index. */
    iter->m_context->m_phrase_index->compact();
    delete iter;
}

bool pinyin_save(pinyin_context_t * context){
    if (!context->m_user_dir)
        return false;

    if (!context->m_modified)
        return false;

    context->m_phrase_index->compact();

    /* skip the reserved zero phrase library. */
    for (size_t i = 1; i < PHRASE_INDEX_LIBRARY_COUNT; ++i) {
        PhraseIndexRange range;
        int retval = context->m_phrase_index->get_range(i, range);

        if (ERROR_NO_SUB_PHRASE_INDEX == retval)
            continue;

        const pinyin_table_info_t * table_info = pinyin_phrase_files + i;

        if (NOT_USED == table_info->m_file_type)
            continue;

        const char * userfilename = table_info->m_user_filename;

        if (NULL == userfilename)
            continue;

        if (SYSTEM_FILE == table_info->m_file_type) {
            /* system phrase library */
            MemoryChunk * chunk = new MemoryChunk;
            MemoryChunk * log = new MemoryChunk;
            const char * systemfilename = table_info->m_system_filename;

            /* check bin file in system dir. */
            gchar * chunkfilename = g_build_filename(context->m_system_dir,
                                                     systemfilename, NULL);
            chunk->load(chunkfilename);
            g_free(chunkfilename);
            context->m_phrase_index->diff(i, chunk, log);

            const char * userfilename = table_info->m_user_filename;
            gchar * tmpfilename = g_strdup_printf("%s.tmp", userfilename);

            gchar * tmppathname = g_build_filename(context->m_user_dir,
                                                   tmpfilename, NULL);
            g_free(tmpfilename);

            gchar * chunkpathname = g_build_filename(context->m_user_dir,
                                                     userfilename, NULL);
            log->save(tmppathname);
            rename(tmppathname, chunkpathname);
            g_free(chunkpathname);
            g_free(tmppathname);
            delete log;
        }

        if (USER_FILE == table_info->m_file_type) {
            /* user phrase library */
            MemoryChunk * chunk = new MemoryChunk;
            context->m_phrase_index->store(i, chunk);

            const char * userfilename = table_info->m_user_filename;
            gchar * tmpfilename = g_strdup_printf("%s.tmp", userfilename);
            gchar * tmppathname = g_build_filename(context->m_user_dir,
                                                   tmpfilename, NULL);
            g_free(tmpfilename);

            gchar * chunkpathname = g_build_filename(context->m_user_dir,
                                                     userfilename, NULL);

            chunk->save(tmppathname);
            rename(tmppathname, chunkpathname);
            g_free(chunkpathname);
            g_free(tmppathname);
            delete chunk;
        }
    }

    /* save user chewing table */
    gchar * tmpfilename = g_build_filename
        (context->m_user_dir, "user_pinyin_index.bin.tmp", NULL);
    unlink(tmpfilename);
    gchar * filename = g_build_filename
        (context->m_user_dir, "user_pinyin_index.bin", NULL);

    MemoryChunk * chunk = new MemoryChunk;
    context->m_pinyin_table->store(chunk);
    chunk->save(tmpfilename);
    delete chunk;
    rename(tmpfilename, filename);
    g_free(tmpfilename);
    g_free(filename);

    /* save user phrase table */
    tmpfilename = g_build_filename
        (context->m_user_dir, "user_phrase_index.bin.tmp", NULL);
    unlink(tmpfilename);
    filename = g_build_filename
        (context->m_user_dir, "user_phrase_index.bin", NULL);

    chunk = new MemoryChunk;
    context->m_phrase_table->store(chunk);
    chunk->save(tmpfilename);
    delete chunk;
    rename(tmpfilename, filename);
    g_free(tmpfilename);
    g_free(filename);

    /* save user bi-gram */
    tmpfilename = g_build_filename
        (context->m_user_dir, "user.db.tmp", NULL);
    unlink(tmpfilename);
    filename = g_build_filename(context->m_user_dir, "user.db", NULL);
    context->m_user_bigram->save_db(tmpfilename);
    rename(tmpfilename, filename);
    g_free(tmpfilename);
    g_free(filename);

    mark_version(context->m_user_dir);

    context->m_modified = false;
    return true;
}

bool pinyin_set_double_pinyin_scheme(pinyin_context_t * context,
                                     DoublePinyinScheme scheme){
    context->m_double_pinyin_parser->set_scheme(scheme);
    return true;
}

bool pinyin_set_chewing_scheme(pinyin_context_t * context,
                               ChewingScheme scheme){
    context->m_chewing_parser->set_scheme(scheme);
    return true;
}


void pinyin_fini(pinyin_context_t * context){
    delete context->m_full_pinyin_parser;
    delete context->m_double_pinyin_parser;
    delete context->m_chewing_parser;
    delete context->m_pinyin_table;
    delete context->m_phrase_table;
    delete context->m_phrase_index;
    delete context->m_system_bigram;
    delete context->m_user_bigram;
    delete context->m_pinyin_lookup;
    delete context->m_phrase_lookup;

    g_free(context->m_system_dir);
    g_free(context->m_user_dir);
    context->m_modified = false;
}

/* copy from options to context->m_options. */
bool pinyin_set_options(pinyin_context_t * context,
                        pinyin_option_t options){
    context->m_options = options;
    context->m_pinyin_table->set_options(context->m_options);
    context->m_pinyin_lookup->set_options(context->m_options);
    return true;
}


pinyin_instance_t * pinyin_alloc_instance(pinyin_context_t * context){
    pinyin_instance_t * instance = new pinyin_instance_t;
    instance->m_context = context;

    instance->m_raw_full_pinyin = NULL;

    instance->m_prefixes = g_array_new(FALSE, FALSE, sizeof(phrase_token_t));
    instance->m_pinyin_keys = g_array_new(FALSE, FALSE, sizeof(ChewingKey));
    instance->m_pinyin_key_rests =
        g_array_new(FALSE, FALSE, sizeof(ChewingKeyRest));
    instance->m_constraints = g_array_new
        (FALSE, FALSE, sizeof(lookup_constraint_t));
    instance->m_match_results =
        g_array_new(FALSE, FALSE, sizeof(phrase_token_t));

    return instance;
}

void pinyin_free_instance(pinyin_instance_t * instance){
    g_free(instance->m_raw_full_pinyin);
    g_array_free(instance->m_prefixes, TRUE);
    g_array_free(instance->m_pinyin_keys, TRUE);
    g_array_free(instance->m_pinyin_key_rests, TRUE);
    g_array_free(instance->m_constraints, TRUE);
    g_array_free(instance->m_match_results, TRUE);

    delete instance;
}


static bool pinyin_update_constraints(pinyin_instance_t * instance){
    pinyin_context_t * & context = instance->m_context;
    ChewingKeyVector & pinyin_keys = instance->m_pinyin_keys;
    CandidateConstraints & constraints = instance->m_constraints;

    size_t key_len = constraints->len;
    g_array_set_size(constraints, pinyin_keys->len);
    for (size_t i = key_len; i < pinyin_keys->len; ++i ) {
        lookup_constraint_t * constraint =
            &g_array_index(constraints, lookup_constraint_t, i);
        constraint->m_type = NO_CONSTRAINT;
    }

    context->m_pinyin_lookup->validate_constraint
        (constraints, pinyin_keys);

    return true;
}


bool pinyin_guess_sentence(pinyin_instance_t * instance){
    pinyin_context_t * & context = instance->m_context;

    g_array_set_size(instance->m_prefixes, 0);
    g_array_append_val(instance->m_prefixes, sentence_start);

    pinyin_update_constraints(instance);
    bool retval = context->m_pinyin_lookup->get_best_match
        (instance->m_prefixes,
         instance->m_pinyin_keys,
         instance->m_constraints,
         instance->m_match_results);

    return retval;
}

bool pinyin_guess_sentence_with_prefix(pinyin_instance_t * instance,
                                       const char * prefix){
    pinyin_context_t * & context = instance->m_context;

    g_array_set_size(instance->m_prefixes, 0);
    g_array_append_val(instance->m_prefixes, sentence_start);

    glong len_str = 0;
    ucs4_t * ucs4_str = g_utf8_to_ucs4(prefix, -1, NULL, &len_str, NULL);

    if (ucs4_str && len_str) {
        /* add prefixes. */
        for (ssize_t i = 1; i <= len_str; ++i) {
            if (i > MAX_PHRASE_LENGTH)
                break;

            phrase_token_t token = null_token;
            ucs4_t * start = ucs4_str + len_str - i;
            int result = context->m_phrase_table->search(i, start, token);
            if (result & SEARCH_OK)
                g_array_append_val(instance->m_prefixes, token);
        }
    }
    g_free(ucs4_str);

    pinyin_update_constraints(instance);
    bool retval = context->m_pinyin_lookup->get_best_match
        (instance->m_prefixes,
         instance->m_pinyin_keys,
         instance->m_constraints,
         instance->m_match_results);

    return retval;
}

bool pinyin_phrase_segment(pinyin_instance_t * instance,
                           const char * sentence){
    pinyin_context_t * & context = instance->m_context;

    const glong num_of_chars = g_utf8_strlen(sentence, -1);
    glong ucs4_len = 0;
    ucs4_t * ucs4_str = g_utf8_to_ucs4(sentence, -1, NULL, &ucs4_len, NULL);

    g_return_val_if_fail(num_of_chars == ucs4_len, FALSE);

    bool retval = context->m_phrase_lookup->get_best_match
        (ucs4_len, ucs4_str, instance->m_match_results);

    g_free(ucs4_str);
    return retval;
}

/* the returned sentence should be freed by g_free(). */
bool pinyin_get_sentence(pinyin_instance_t * instance,
                         char ** sentence){
    pinyin_context_t * & context = instance->m_context;

    bool retval = pinyin::convert_to_utf8
        (context->m_phrase_index, instance->m_match_results,
         NULL, *sentence);

    return retval;
}

bool pinyin_parse_full_pinyin(pinyin_instance_t * instance,
                              const char * onepinyin,
                              ChewingKey * onekey){
    pinyin_context_t * & context = instance->m_context;

    int pinyin_len = strlen(onepinyin);
    int parse_len = context->m_full_pinyin_parser->parse_one_key
        ( context->m_options, *onekey, onepinyin, pinyin_len);
    return pinyin_len == parse_len;
}

size_t pinyin_parse_more_full_pinyins(pinyin_instance_t * instance,
                                      const char * pinyins){
    pinyin_context_t * & context = instance->m_context;

    g_free(instance->m_raw_full_pinyin);
    instance->m_raw_full_pinyin = g_strdup(pinyins);
    int pinyin_len = strlen(pinyins);

    int parse_len = context->m_full_pinyin_parser->parse
        ( context->m_options, instance->m_pinyin_keys,
          instance->m_pinyin_key_rests, pinyins, pinyin_len);

    return parse_len;
}

bool pinyin_parse_double_pinyin(pinyin_instance_t * instance,
                                const char * onepinyin,
                                ChewingKey * onekey){
    pinyin_context_t * & context = instance->m_context;

    int pinyin_len = strlen(onepinyin);
    int parse_len = context->m_double_pinyin_parser->parse_one_key
        ( context->m_options, *onekey, onepinyin, pinyin_len);
    return pinyin_len == parse_len;
}

size_t pinyin_parse_more_double_pinyins(pinyin_instance_t * instance,
                                        const char * pinyins){
    pinyin_context_t * & context = instance->m_context;
    int pinyin_len = strlen(pinyins);

    int parse_len = context->m_double_pinyin_parser->parse
        ( context->m_options, instance->m_pinyin_keys,
          instance->m_pinyin_key_rests, pinyins, pinyin_len);

    return parse_len;
}

bool pinyin_parse_chewing(pinyin_instance_t * instance,
                          const char * onechewing,
                          ChewingKey * onekey){
    pinyin_context_t * & context = instance->m_context;

    int chewing_len = strlen(onechewing);
    int parse_len = context->m_chewing_parser->parse_one_key
        ( context->m_options, *onekey, onechewing, chewing_len );
    return chewing_len == parse_len;
}

size_t pinyin_parse_more_chewings(pinyin_instance_t * instance,
                                  const char * chewings){
    pinyin_context_t * & context = instance->m_context;
    int chewing_len = strlen(chewings);

    int parse_len = context->m_chewing_parser->parse
        ( context->m_options, instance->m_pinyin_keys,
          instance->m_pinyin_key_rests, chewings, chewing_len);

    return parse_len;
}

bool pinyin_in_chewing_keyboard(pinyin_instance_t * instance,
                                const char key, const char ** symbol) {
    pinyin_context_t * & context = instance->m_context;
    return context->m_chewing_parser->in_chewing_scheme
        (context->m_options, key, symbol);
}

#if 0
static gint compare_item_with_token(gconstpointer lhs,
                                    gconstpointer rhs) {
    lookup_candidate_t * item_lhs = (lookup_candidate_t *)lhs;
    lookup_candidate_t * item_rhs = (lookup_candidate_t *)rhs;

    phrase_token_t token_lhs = item_lhs->m_token;
    phrase_token_t token_rhs = item_rhs->m_token;

    return (token_lhs - token_rhs);
}
#endif

static gint compare_item_with_frequency(gconstpointer lhs,
                                        gconstpointer rhs) {
    lookup_candidate_t * item_lhs = (lookup_candidate_t *)lhs;
    lookup_candidate_t * item_rhs = (lookup_candidate_t *)rhs;

    guint32 freq_lhs = item_lhs->m_freq;
    guint32 freq_rhs = item_rhs->m_freq;

    return -(freq_lhs - freq_rhs); /* in descendant order */
}

static phrase_token_t _get_previous_token(pinyin_instance_t * instance,
                                          size_t offset) {
    phrase_token_t prev_token = null_token;
    ssize_t i;

    if (0 == offset) {
        /* get previous token from prefixes. */
        prev_token = sentence_start;
        size_t prev_token_len = 0;

        pinyin_context_t * context = instance->m_context;
        TokenVector prefixes = instance->m_prefixes;
        PhraseItem item;

        for (size_t i = 0; i < prefixes->len; ++i) {
            phrase_token_t token = g_array_index(prefixes, phrase_token_t, i);
            if (sentence_start == token)
                continue;

            int retval = context->m_phrase_index->get_phrase_item(token, item);
            if (ERROR_OK == retval) {
                size_t token_len = item.get_phrase_length();
                if (token_len > prev_token_len) {
                    /* found longer match, and save it. */
                    prev_token = token;
                    prev_token_len = token_len;
                }
            }
        }
    } else {
        /* get previous token from match results. */
        assert (0 < offset);

        phrase_token_t cur_token = g_array_index
            (instance->m_match_results, phrase_token_t, offset);
        if (null_token != cur_token) {
            for (i = offset - 1; i >= 0; --i) {
                cur_token = g_array_index
                    (instance->m_match_results, phrase_token_t, i);
                if (null_token != cur_token) {
                    prev_token = cur_token;
                    break;
                }
            }
        }
    }

    return prev_token;
}

static void _append_items(pinyin_context_t * context,
                          PhraseIndexRanges ranges,
                          lookup_candidate_t * template_item,
                          CandidateVector items) {
    /* reduce and append to a single GArray. */
    for (size_t m = 0; m < PHRASE_INDEX_LIBRARY_COUNT; ++m) {
        if (NULL == ranges[m])
            continue;

        for (size_t n = 0; n < ranges[m]->len; ++n) {
            PhraseIndexRange * range =
                &g_array_index(ranges[m], PhraseIndexRange, n);
            for (size_t k = range->m_range_begin;
                 k < range->m_range_end; ++k) {
                lookup_candidate_t item;
                item.m_candidate_type = template_item->m_candidate_type;
                item.m_token = k;
                item.m_orig_rest = template_item->m_orig_rest;
                item.m_new_pinyins = g_strdup(template_item->m_new_pinyins);
                item.m_freq = template_item->m_freq;
                g_array_append_val(items, item);
            }
        }
    }
}

#if 0
static void _remove_duplicated_items(CandidateVector items) {
    /* remove the duplicated items. */
    phrase_token_t last_token = null_token, saved_token;
    for (size_t n = 0; n < items->len; ++n) {
        lookup_candidate_t * item = &g_array_index
            (items, lookup_candidate_t, n);

        saved_token = item->m_token;
        if (last_token == saved_token) {
            g_array_remove_index(items, n);
            n--;
        }
        last_token = saved_token;
    }
}
#endif

static void _compute_frequency_of_items(pinyin_context_t * context,
                                        phrase_token_t prev_token,
                                        SingleGram * merged_gram,
                                        CandidateVector items) {
    pinyin_option_t & options = context->m_options;
    ssize_t i;

    PhraseItem cached_item;
    /* compute all freqs. */
    for (i = 0; i < items->len; ++i) {
        lookup_candidate_t * item = &g_array_index
            (items, lookup_candidate_t, i);
        phrase_token_t & token = item->m_token;

        gfloat bigram_poss = 0; guint32 total_freq = 0;
        if (options & DYNAMIC_ADJUST) {
            if (null_token != prev_token) {
                guint32 bigram_freq = 0;
                merged_gram->get_total_freq(total_freq);
                merged_gram->get_freq(token, bigram_freq);
                if (0 != total_freq)
                    bigram_poss = bigram_freq / (gfloat)total_freq;
            }
        }

        /* compute the m_freq. */
        FacadePhraseIndex * & phrase_index = context->m_phrase_index;
        phrase_index->get_phrase_item(token, cached_item);
        total_freq = phrase_index->get_phrase_index_total_freq();
        assert (0 < total_freq);

        /* Note: possibility value <= 1.0. */
        guint32 freq = (LAMBDA_PARAMETER * bigram_poss +
                        (1 - LAMBDA_PARAMETER) *
                        cached_item.get_unigram_frequency() /
                        (gfloat) total_freq) * 256 * 256 * 256;
        item->m_freq = freq;
    }
}

static bool _prepend_sentence_candidate(CandidateVector candidates) {
    /* prepend best match candidate to candidates. */

    lookup_candidate_t candidate;
    candidate.m_candidate_type = BEST_MATCH_CANDIDATE;
    g_array_prepend_val(candidates, candidate);

    return true;
}

static bool _compute_phrase_strings_of_items(pinyin_instance_t * instance,
                                             CandidateVector candidates) {
    /* populate m_phrase_string in lookup_candidate_t. */

    for(size_t i = 0; i < candidates->len; ++i) {
        lookup_candidate_t * candidate = &g_array_index
            (candidates, lookup_candidate_t, i);

        switch(candidate->m_candidate_type) {
        case BEST_MATCH_CANDIDATE:
            pinyin_get_sentence(instance, &(candidate->m_phrase_string));
            break;
        case NORMAL_CANDIDATE:
        case DIVIDED_CANDIDATE:
        case RESPLIT_CANDIDATE:
            pinyin_translate_token
                (instance, candidate->m_token, &(candidate->m_phrase_string));
            break;
        case ZOMBIE_CANDIDATE:
            break;
        }
    }

    return true;
}

static gint compare_indexed_item_with_phrase_string(gconstpointer lhs,
                                                    gconstpointer rhs,
                                                    gpointer userdata) {
    size_t index_lhs = *((size_t *) lhs);
    size_t index_rhs = *((size_t *) rhs);
    CandidateVector candidates = (CandidateVector) userdata;

    lookup_candidate_t * candidate_lhs =
        &g_array_index(candidates, lookup_candidate_t, index_lhs);
    lookup_candidate_t * candidate_rhs =
        &g_array_index(candidates, lookup_candidate_t, index_rhs);

    return -strcmp(candidate_lhs->m_phrase_string,
                   candidate_rhs->m_phrase_string); /* in descendant order */
}


static bool _remove_duplicated_items_by_phrase_string
(pinyin_instance_t * instance,
 CandidateVector candidates) {
    size_t i;
    /* create the GArray of indexed item */
    GArray * indices = g_array_new(FALSE, FALSE, sizeof(size_t));
    for (i = 0; i < candidates->len; ++i)
        g_array_append_val(indices, i);

    /* sort the indices array by phrase array */
    g_array_sort_with_data
        (indices, compare_indexed_item_with_phrase_string, candidates);

    /* mark duplicated items as zombie candidate */
    lookup_candidate_t * cur_item, * saved_item = NULL;
    for (i = 0; i < indices->len; ++i) {
        size_t cur_index = g_array_index(indices, size_t, i);
        cur_item = &g_array_index(candidates, lookup_candidate_t, cur_index);

        if (!saved_item) {
            saved_item = cur_item;
            continue;
        }

        if (0 == strcmp(saved_item->m_phrase_string,
                        cur_item->m_phrase_string)) {
            /* found duplicated candidates */

            /* keep best match candidate */
            if (BEST_MATCH_CANDIDATE == saved_item->m_candidate_type) {
                cur_item->m_candidate_type = ZOMBIE_CANDIDATE;
                continue;
            }

            if (BEST_MATCH_CANDIDATE == cur_item->m_candidate_type) {
                saved_item->m_candidate_type = ZOMBIE_CANDIDATE;
                saved_item = cur_item;
                continue;
            }

            /* keep the higher possiblity one
               to quickly move the word forward in the candidate list */
            if (cur_item->m_freq > saved_item->m_freq) {
                /* find better candidate */
                saved_item->m_candidate_type = ZOMBIE_CANDIDATE;
                saved_item = cur_item;
                continue;
            } else {
                cur_item->m_candidate_type = ZOMBIE_CANDIDATE;
                continue;
            }
        } else {
            /* keep the current candidate */
            saved_item = cur_item;
        }
    }

    g_array_free(indices, TRUE);

    /* remove zombie candidate from the returned candidates */
    for (i = 0; i < candidates->len; ++i) {
        lookup_candidate_t * candidate = &g_array_index
            (candidates, lookup_candidate_t, i);

        if (ZOMBIE_CANDIDATE == candidate->m_candidate_type) {
            g_array_remove_index(candidates, i);
            i--;
        }
    }

    return true;
}

static bool _free_candidates(CandidateVector candidates) {
    /* free candidates */
    for (size_t i = 0; i < candidates->len; ++i) {
        lookup_candidate_t * candidate = &g_array_index
            (candidates, lookup_candidate_t, i);
        g_free(candidate->m_phrase_string);
        g_free(candidate->m_new_pinyins);
    }
    g_array_set_size(candidates, 0);

    return true;
}

bool pinyin_get_candidates(pinyin_instance_t * instance,
                           size_t offset,
                           CandidateVector candidates) {

    pinyin_context_t * & context = instance->m_context;
    pinyin_option_t & options = context->m_options;
    ChewingKeyVector & pinyin_keys = instance->m_pinyin_keys;

    _free_candidates(candidates);

    size_t pinyin_len = pinyin_keys->len - offset;
    ssize_t i;

    /* lookup the previous token here. */
    phrase_token_t prev_token = null_token;

    if (options & DYNAMIC_ADJUST) {
        prev_token = _get_previous_token(instance, offset);
    }

    SingleGram merged_gram;
    SingleGram * system_gram = NULL, * user_gram = NULL;

    if (options & DYNAMIC_ADJUST) {
        if (null_token != prev_token) {
            context->m_system_bigram->load(prev_token, system_gram);
            context->m_user_bigram->load(prev_token, user_gram);
            merge_single_gram(&merged_gram, system_gram, user_gram);
        }
    }

    PhraseIndexRanges ranges;
    memset(ranges, 0, sizeof(ranges));
    context->m_phrase_index->prepare_ranges(ranges);

    GArray * items = g_array_new(FALSE, FALSE, sizeof(lookup_candidate_t));

    for (i = pinyin_len; i >= 1; --i) {
        g_array_set_size(items, 0);

        ChewingKey * keys = &g_array_index
            (pinyin_keys, ChewingKey, offset);

        /* do pinyin search. */
        int retval = context->m_pinyin_table->search
            (i, keys, ranges);

        if ( !(retval & SEARCH_OK) )
            continue;

        lookup_candidate_t template_item;
        _append_items(context, ranges, &template_item, items);

#if 0
        g_array_sort(items, compare_item_with_token);

        _remove_duplicated_items(items);
#endif

        _compute_frequency_of_items(context, prev_token, &merged_gram, items);

        /* sort the candidates of the same length by frequency. */
        g_array_sort(items, compare_item_with_frequency);

        /* transfer back items to tokens, and save it into candidates */
        for (size_t k = 0; k < items->len; ++k) {
            lookup_candidate_t * item = &g_array_index
                (items, lookup_candidate_t, k);
            g_array_append_val(candidates, *item);
        }

        if (!(retval & SEARCH_CONTINUED))
            break;
    }

    g_array_free(items, TRUE);
    context->m_phrase_index->destroy_ranges(ranges);
    if (system_gram)
        delete system_gram;
    if (user_gram)
        delete user_gram;

    /* post process to remove duplicated candidates */

    _prepend_sentence_candidate(candidates);

    _compute_phrase_strings_of_items(instance, candidates);

    _remove_duplicated_items_by_phrase_string(instance, candidates);

    return true;
}


static bool _try_divided_table(pinyin_instance_t * instance,
                               PhraseIndexRanges ranges,
                               size_t offset,
                               CandidateVector items){
    bool found = false;

    pinyin_context_t * & context = instance->m_context;
    pinyin_option_t & options = context->m_options;
    ChewingKeyVector & pinyin_keys = instance->m_pinyin_keys;
    ChewingKeyRestVector & pinyin_key_rests = instance->m_pinyin_key_rests;

    assert(pinyin_keys->len == pinyin_key_rests->len);
    guint num_keys = pinyin_keys->len;
    assert(offset < num_keys);

    /* handle "^xian$" -> "xi'an" here */
    ChewingKey * key = &g_array_index(pinyin_keys, ChewingKey, offset);
    ChewingKeyRest * rest = &g_array_index(pinyin_key_rests,
                                           ChewingKeyRest, offset);
    ChewingKeyRest orig_rest = *rest;
    guint16 tone = CHEWING_ZERO_TONE;

    const divided_table_item_t * item = NULL;

    /* back up tone */
    if (options & USE_TONE) {
        tone = key->m_tone;
        if (CHEWING_ZERO_TONE != tone) {
            key->m_tone = CHEWING_ZERO_TONE;
            rest->m_raw_end --;
        }
    }

    item = context->m_full_pinyin_parser->retrieve_divided_item
        (options, key, rest, instance->m_raw_full_pinyin,
         strlen(instance->m_raw_full_pinyin));

    if (item) {
        /* no ops */
        assert(item->m_new_freq > 0);

        ChewingKey divided_keys[2];
        const char * pinyin = item->m_new_keys[0];
        assert(context->m_full_pinyin_parser->
               parse_one_key(options, divided_keys[0],
                             pinyin, strlen(pinyin)));
        pinyin = item->m_new_keys[1];
        assert(context->m_full_pinyin_parser->
               parse_one_key(options, divided_keys[1],
                             pinyin, strlen(pinyin)));

        gchar * new_pinyins = g_strdup_printf
            ("%s'%s", item->m_new_keys[0], item->m_new_keys[1]);

        /* propagate the tone */
        if (options & USE_TONE) {
            if (CHEWING_ZERO_TONE != tone) {
                assert(0 < tone && tone <= 5);
                divided_keys[1].m_tone = tone;

                gchar * tmp_str = g_strdup_printf
                    ("%s%d", new_pinyins, tone);
                g_free(new_pinyins);
                new_pinyins = tmp_str;
            }
        }

        /* do pinyin search. */
        int retval = context->m_pinyin_table->search
            (2, divided_keys, ranges);

        if (retval & SEARCH_OK) {
            lookup_candidate_t template_item;
            template_item.m_candidate_type = DIVIDED_CANDIDATE;
            template_item.m_orig_rest = orig_rest;
            template_item.m_new_pinyins = new_pinyins;

            _append_items(context, ranges, &template_item, items);
            found = true;
        }
        g_free(new_pinyins);
    }

    /* restore tones */
    if (options & USE_TONE) {
        if (CHEWING_ZERO_TONE != tone) {
            key->m_tone = tone;
            rest->m_raw_end ++;
        }
    }

    return found;
}

static bool _try_resplit_table(pinyin_instance_t * instance,
                               PhraseIndexRanges ranges,
                               size_t offset,
                               CandidateVector items){
    bool found = false;

    pinyin_context_t * & context = instance->m_context;
    pinyin_option_t & options = context->m_options;
    ChewingKeyVector & pinyin_keys = instance->m_pinyin_keys;
    ChewingKeyRestVector & pinyin_key_rests = instance->m_pinyin_key_rests;

    assert(pinyin_keys->len == pinyin_key_rests->len);
    guint num_keys = pinyin_keys->len;
    assert(offset + 1 < num_keys);

    guint16 next_tone = CHEWING_ZERO_TONE;

    /* handle "^fa'nan$" -> "fan'an" here */
    ChewingKeyRest * cur_rest = &g_array_index(pinyin_key_rests,
                                               ChewingKeyRest, offset);
    ChewingKeyRest * next_rest = &g_array_index(pinyin_key_rests,
                                                ChewingKeyRest, offset + 1);
    /* some "'" here */
    if (cur_rest->m_raw_end != next_rest->m_raw_begin)
        return found;

    ChewingKey * cur_key = &g_array_index(pinyin_keys, ChewingKey, offset);
    ChewingKey * next_key = &g_array_index(pinyin_keys, ChewingKey,
                                           offset + 1);

    /* some tone here */
    if (CHEWING_ZERO_TONE != cur_key->m_tone)
        return found;

    ChewingKeyRest orig_rest;
    orig_rest.m_raw_begin = cur_rest->m_raw_begin;
    orig_rest.m_raw_end = next_rest->m_raw_end;

    /* backup tone */
    if (options & USE_TONE) {
        next_tone = next_key->m_tone;
        if (CHEWING_ZERO_TONE != next_tone) {
            next_key->m_tone = CHEWING_ZERO_TONE;
            next_rest->m_raw_end --;
        }
    }

    /* lookup re-split table */
    const char * str = instance->m_raw_full_pinyin;
    const resplit_table_item_t * item_by_orig =
        context->m_full_pinyin_parser->
        retrieve_resplit_item_by_original_pinyins
        (options, cur_key, cur_rest, next_key, next_rest, str, strlen(str));

    const resplit_table_item_t * item_by_new =
        context->m_full_pinyin_parser->
        retrieve_resplit_item_by_resplit_pinyins
        (options, cur_key, cur_rest, next_key, next_rest, str, strlen(str));

    /* there are no same couple of pinyins in re-split table. */
    assert(!(item_by_orig && item_by_new));

    ChewingKey resplit_keys[2];
    const char * pinyins[2];

    bool tosearch = false;
    if (item_by_orig && item_by_orig->m_new_freq) {
        pinyins[0] = item_by_orig->m_new_keys[0];
        pinyins[1] = item_by_orig->m_new_keys[1];

        assert(context->m_full_pinyin_parser->
               parse_one_key(options, resplit_keys[0],
                             pinyins[0], strlen(pinyins[0])));

        assert(context->m_full_pinyin_parser->
               parse_one_key(options, resplit_keys[1],
                             pinyins[1], strlen(pinyins[1])));
        tosearch = true;
    }

    if (item_by_new && item_by_new->m_orig_freq) {
        pinyins[0] = item_by_new->m_orig_keys[0];
        pinyins[1] = item_by_new->m_orig_keys[1];

        assert(context->m_full_pinyin_parser->
               parse_one_key(options, resplit_keys[0],
                             pinyins[0], strlen(pinyins[0])));

        assert(context->m_full_pinyin_parser->
               parse_one_key(options, resplit_keys[1],
                             pinyins[1], strlen(pinyins[1])));
        tosearch = true;
    }

    if (tosearch) {
        gchar * new_pinyins = g_strdup_printf
            ("%s'%s", pinyins[0], pinyins[1]);

        /* propagate the tone */
        if (options & USE_TONE) {
            if (CHEWING_ZERO_TONE != next_tone) {
                assert(0 < next_tone && next_tone <= 5);
                resplit_keys[1].m_tone = next_tone;

                gchar * tmp_str = g_strdup_printf
                    ("%s%d", new_pinyins, next_tone);
                g_free(new_pinyins);
                new_pinyins = tmp_str;
            }
        }

        /* do pinyin search. */
        int retval = context->m_pinyin_table->search
            (2, resplit_keys, ranges);

        if (retval & SEARCH_OK) {
            lookup_candidate_t template_item;
            template_item.m_candidate_type = RESPLIT_CANDIDATE;
            template_item.m_orig_rest = orig_rest;
            template_item.m_new_pinyins = new_pinyins;

            _append_items(context, ranges, &template_item, items);
            found = true;
        }
        g_free(new_pinyins);
    }

    /* restore tones */
    if (options & USE_TONE) {
        if (CHEWING_ZERO_TONE != next_tone) {
            next_key->m_tone = next_tone;
            next_rest->m_raw_end ++;
        }
    }

    return found;
}

bool pinyin_get_full_pinyin_candidates(pinyin_instance_t * instance,
                                       size_t offset,
                                       CandidateVector candidates){

    pinyin_context_t * & context = instance->m_context;
    pinyin_option_t & options = context->m_options;
    ChewingKeyVector & pinyin_keys = instance->m_pinyin_keys;

    _free_candidates(candidates);

    size_t pinyin_len = pinyin_keys->len - offset;
    pinyin_len = std_lite::min((size_t)MAX_PHRASE_LENGTH, pinyin_len);
    ssize_t i;

    /* lookup the previous token here. */
    phrase_token_t prev_token = null_token;

    if (options & DYNAMIC_ADJUST) {
        prev_token = _get_previous_token(instance, offset);
    }

    SingleGram merged_gram;
    SingleGram * system_gram = NULL, * user_gram = NULL;

    if (options & DYNAMIC_ADJUST) {
        if (null_token != prev_token) {
            context->m_system_bigram->load(prev_token, system_gram);
            context->m_user_bigram->load(prev_token, user_gram);
            merge_single_gram(&merged_gram, system_gram, user_gram);
        }
    }

    PhraseIndexRanges ranges;
    memset(ranges, 0, sizeof(ranges));
    context->m_phrase_index->prepare_ranges(ranges);

    GArray * items = g_array_new(FALSE, FALSE, sizeof(lookup_candidate_t));

    if (1 == pinyin_len) {
        /* because there is only one pinyin left,
         *  the following for-loop will not produce 2 character candidates.
         * the if-branch will fill the candidate list with
         *  2 character candidates.
         */

        if (options & USE_DIVIDED_TABLE) {
            g_array_set_size(items, 0);

            if (_try_divided_table(instance, ranges, offset, items)) {

#if 0
                g_array_sort(items, compare_item_with_token);

                _remove_duplicated_items(items);
#endif

                _compute_frequency_of_items(context, prev_token,
                                            &merged_gram, items);

                /* sort the candidates of the same length by frequency. */
                g_array_sort(items, compare_item_with_frequency);

                /* transfer back items to tokens, and save it into candidates */
                for (i = 0; i < items->len; ++i) {
                    lookup_candidate_t * item = &g_array_index
                        (items, lookup_candidate_t, i);
                    g_array_append_val(candidates, *item);
                }
            }
        }
    }

    for (i = pinyin_len; i >= 1; --i) {
        bool found = false;
        g_array_set_size(items, 0);

        if (2 == i) {
            /* handle fuzzy pinyin segment here. */
            if (options & USE_DIVIDED_TABLE) {
                found = _try_divided_table(instance, ranges, offset, items) ||
                    found;
            }
            if (options & USE_RESPLIT_TABLE) {
                found = _try_resplit_table(instance, ranges, offset, items) ||
                    found;
            }
        }

        ChewingKey * keys = &g_array_index
            (pinyin_keys, ChewingKey, offset);

        /* do pinyin search. */
        int retval = context->m_pinyin_table->search
            (i, keys, ranges);

        found = (retval & SEARCH_OK) || found;

        if ( !found )
            continue;

        lookup_candidate_t template_item;
        _append_items(context, ranges, &template_item, items);

#if 0
        g_array_sort(items, compare_item_with_token);

        _remove_duplicated_items(items);
#endif

        _compute_frequency_of_items(context, prev_token, &merged_gram, items);

        g_array_sort(items, compare_item_with_frequency);

        for (size_t k = 0; k < items->len; ++k) {
            lookup_candidate_t * item = &g_array_index
                (items, lookup_candidate_t, k);
            g_array_append_val(candidates, *item);
        }

        if (!(retval & SEARCH_CONTINUED))
            break;
    }

    g_array_free(items, TRUE);
    context->m_phrase_index->destroy_ranges(ranges);
    if (system_gram)
        delete system_gram;
    if (user_gram)
        delete user_gram;

    /* post process to remove duplicated candidates */

    _prepend_sentence_candidate(candidates);

    _compute_phrase_strings_of_items(instance, candidates);

    _remove_duplicated_items_by_phrase_string(instance, candidates);

    return true;
}


int pinyin_choose_candidate(pinyin_instance_t * instance,
                            size_t offset,
                            lookup_candidate_t * candidate){
    pinyin_context_t * & context = instance->m_context;

    if (DIVIDED_CANDIDATE == candidate->m_candidate_type ||
        RESPLIT_CANDIDATE == candidate->m_candidate_type) {
        /* update full pinyin. */
        gchar * oldpinyins = instance->m_raw_full_pinyin;
        const ChewingKeyRest rest = candidate->m_orig_rest;
        oldpinyins[rest.m_raw_begin] = '\0';
        const gchar * left_part = oldpinyins;
        const gchar * right_part = oldpinyins + rest.m_raw_end;
        gchar * newpinyins = g_strconcat(left_part, candidate->m_new_pinyins,
                                         right_part, NULL);
        g_free(oldpinyins);
        instance->m_raw_full_pinyin = newpinyins;

        /* re-parse the full pinyin.  */
        const gchar * pinyins = instance->m_raw_full_pinyin;
        int pinyin_len = strlen(pinyins);
        int parse_len = context->m_full_pinyin_parser->parse
            (context->m_options, instance->m_pinyin_keys,
             instance->m_pinyin_key_rests, pinyins, pinyin_len);

        /* Note: there may be some un-parsable input here. */
    }

    /* sync m_constraints to the length of m_pinyin_keys. */
    bool retval = context->m_pinyin_lookup->validate_constraint
        (instance->m_constraints, instance->m_pinyin_keys);

    phrase_token_t token = candidate->m_token;
    guint8 len = context->m_pinyin_lookup->add_constraint
        (instance->m_constraints, offset, token);

    /* safe guard: validate the m_constraints again. */
    retval = context->m_pinyin_lookup->validate_constraint
        (instance->m_constraints, instance->m_pinyin_keys) && len;

    return offset + len;
}


bool pinyin_free_candidates(pinyin_instance_t * instance,
                            CandidateVector candidates) {
    _free_candidates(candidates);
    g_array_free(candidates, TRUE);
    return true;
}


bool pinyin_clear_constraint(pinyin_instance_t * instance,
                             size_t offset){
    pinyin_context_t * & context = instance->m_context;

    bool retval = context->m_pinyin_lookup->clear_constraint
        (instance->m_constraints, offset);

    return retval;
}

bool pinyin_clear_constraints(pinyin_instance_t * instance){
    pinyin_context_t * & context = instance->m_context;
    bool retval = true;

    for ( size_t i = 0; i < instance->m_constraints->len; ++i ) {
        retval = context->m_pinyin_lookup->clear_constraint
            (instance->m_constraints, i) && retval;
    }

    return retval;
}

/* the returned word should be freed by g_free. */
bool pinyin_translate_token(pinyin_instance_t * instance,
                            phrase_token_t token, char ** word){
    pinyin_context_t * & context = instance->m_context;
    PhraseItem item;
    ucs4_t buffer[MAX_PHRASE_LENGTH];

    int retval = context->m_phrase_index->get_phrase_item(token, item);
    item.get_phrase_string(buffer);
    guint8 length = item.get_phrase_length();
    *word = g_ucs4_to_utf8(buffer, length, NULL, NULL, NULL);
    return ERROR_OK == retval;
}

bool pinyin_train(pinyin_instance_t * instance){
    if (!instance->m_context->m_user_dir)
        return false;

    pinyin_context_t * & context = instance->m_context;
    context->m_modified = true;

    bool retval = context->m_pinyin_lookup->train_result2
        (instance->m_pinyin_keys, instance->m_constraints,
         instance->m_match_results);

    return retval;
}

bool pinyin_reset(pinyin_instance_t * instance){
    g_array_set_size(instance->m_pinyin_keys, 0);
    g_array_set_size(instance->m_pinyin_key_rests, 0);
    g_array_set_size(instance->m_constraints, 0);
    g_array_set_size(instance->m_match_results, 0);

    return true;
}

/**
 *  Note: prefix is the text before the pre-edit string.
 */
