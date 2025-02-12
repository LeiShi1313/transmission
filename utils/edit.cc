/*
 * This file Copyright (C) 2012-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <stdio.h> /* fprintf() */
#include <string.h> /* strlen(), strstr(), strcmp() */
#include <stdlib.h> /* EXIT_FAILURE */

#include <event2/buffer.h>

#include <libtransmission/transmission.h>
#include <libtransmission/error.h>
#include <libtransmission/tr-getopt.h>
#include <libtransmission/utils.h>
#include <libtransmission/variant.h>
#include <libtransmission/version.h>

#define MY_NAME "transmission-edit"

static int fileCount = 0;
static bool showVersion = false;
static char const** files = nullptr;
static char const* add = nullptr;
static char const* deleteme = nullptr;
static char const* replace[2] = { nullptr, nullptr };

static tr_option options[] = {
    { 'a', "add", "Add a tracker's announce URL", "a", true, "<url>" },
    { 'd', "delete", "Delete a tracker's announce URL", "d", true, "<url>" },
    { 'r', "replace", "Search and replace a substring in the announce URLs", "r", true, "<old> <new>" },
    { 'V', "version", "Show version number and exit", "V", false, nullptr },
    { 0, nullptr, nullptr, nullptr, false, nullptr }
};

static char const* getUsage(void)
{
    return "Usage: " MY_NAME " [options] torrent-file(s)";
}

static int parseCommandLine(int argc, char const* const* argv)
{
    int c;
    char const* optarg;

    while ((c = tr_getopt(getUsage(), argc, argv, options, &optarg)) != TR_OPT_DONE)
    {
        switch (c)
        {
        case 'a':
            add = optarg;
            break;

        case 'd':
            deleteme = optarg;
            break;

        case 'r':
            replace[0] = optarg;
            c = tr_getopt(getUsage(), argc, argv, options, &optarg);

            if (c != TR_OPT_UNK)
            {
                return 1;
            }

            replace[1] = optarg;
            break;

        case 'V':
            showVersion = true;
            break;

        case TR_OPT_UNK:
            files[fileCount++] = optarg;
            break;

        default:
            return 1;
        }
    }

    return 0;
}

static bool removeURL(tr_variant* metainfo, char const* url)
{
    char const* str;
    tr_variant* announce_list;
    bool changed = false;

    if (tr_variantDictFindStr(metainfo, TR_KEY_announce, &str, nullptr) && strcmp(str, url) == 0)
    {
        printf("\tRemoved \"%s\" from \"announce\"\n", str);
        tr_variantDictRemove(metainfo, TR_KEY_announce);
        changed = true;
    }

    if (tr_variantDictFindList(metainfo, TR_KEY_announce_list, &announce_list))
    {
        tr_variant* tier;
        int tierIndex = 0;

        while ((tier = tr_variantListChild(announce_list, tierIndex)) != nullptr)
        {
            int nodeIndex = 0;
            tr_variant const* node;
            while ((node = tr_variantListChild(tier, nodeIndex)) != nullptr)
            {
                if (tr_variantGetStr(node, &str, nullptr) && strcmp(str, url) == 0)
                {
                    printf("\tRemoved \"%s\" from \"announce-list\" tier #%d\n", str, tierIndex + 1);
                    tr_variantListRemove(tier, nodeIndex);
                    changed = true;
                }
                else
                {
                    ++nodeIndex;
                }
            }

            if (tr_variantListSize(tier) == 0)
            {
                printf("\tNo URLs left in tier #%d... removing tier\n", tierIndex + 1);
                tr_variantListRemove(announce_list, tierIndex);
            }
            else
            {
                ++tierIndex;
            }
        }

        if (tr_variantListSize(announce_list) == 0)
        {
            printf("\tNo tiers left... removing announce-list\n");
            tr_variantDictRemove(metainfo, TR_KEY_announce_list);
        }
    }

    /* if we removed the "announce" field and there's still another track left,
     * use it as the "announce" field */
    if (changed && !tr_variantDictFindStr(metainfo, TR_KEY_announce, &str, nullptr))
    {
        tr_variant* const tier = tr_variantListChild(announce_list, 0);
        if (tier != nullptr)
        {
            tr_variant const* const node = tr_variantListChild(tier, 0);
            if ((node != nullptr) && tr_variantGetStr(node, &str, nullptr))
            {
                tr_variantDictAddStr(metainfo, TR_KEY_announce, str);
                printf("\tAdded \"%s\" to announce\n", str);
            }
        }
    }

    return changed;
}

static char* replaceSubstr(char const* str, char const* in, char const* out)
{
    char const* walk;
    struct evbuffer* const buf = evbuffer_new();
    size_t const inlen = strlen(in);
    size_t const outlen = strlen(out);

    while ((walk = strstr(str, in)) != nullptr)
    {
        evbuffer_add(buf, str, walk - str);
        evbuffer_add(buf, out, outlen);
        str = walk + inlen;
    }

    evbuffer_add(buf, str, strlen(str));

    return evbuffer_free_to_str(buf, nullptr);
}

static bool replaceURL(tr_variant* metainfo, char const* in, char const* out)
{
    char const* str;
    tr_variant* announce_list;
    bool changed = false;

    if (tr_variantDictFindStr(metainfo, TR_KEY_announce, &str, nullptr) && strstr(str, in) != nullptr)
    {
        char* newstr = replaceSubstr(str, in, out);
        printf("\tReplaced in \"announce\": \"%s\" --> \"%s\"\n", str, newstr);
        tr_variantDictAddStr(metainfo, TR_KEY_announce, newstr);
        tr_free(newstr);
        changed = true;
    }

    if (tr_variantDictFindList(metainfo, TR_KEY_announce_list, &announce_list))
    {
        tr_variant* tier;
        int tierCount = 0;

        while ((tier = tr_variantListChild(announce_list, tierCount)) != nullptr)
        {
            tr_variant* node;
            int nodeCount = 0;

            while ((node = tr_variantListChild(tier, nodeCount)) != nullptr)
            {
                if (tr_variantGetStr(node, &str, nullptr) && strstr(str, in) != nullptr)
                {
                    char* newstr = replaceSubstr(str, in, out);
                    printf("\tReplaced in \"announce-list\" tier %d: \"%s\" --> \"%s\"\n", tierCount + 1, str, newstr);
                    tr_variantFree(node);
                    tr_variantInitStr(node, newstr);
                    tr_free(newstr);
                    changed = true;
                }

                ++nodeCount;
            }

            ++tierCount;
        }
    }

    return changed;
}

static bool announce_list_has_url(tr_variant* announce_list, char const* url)
{
    int tierCount = 0;
    tr_variant* tier;

    while ((tier = tr_variantListChild(announce_list, tierCount)) != nullptr)
    {
        int nodeCount = 0;
        tr_variant const* node;

        while ((node = tr_variantListChild(tier, nodeCount)) != nullptr)
        {
            char const* str = nullptr;
            if (tr_variantGetStr(node, &str, nullptr) && strcmp(str, url) == 0)
            {
                return true;
            }

            ++nodeCount;
        }

        ++tierCount;
    }

    return false;
}

static bool addURL(tr_variant* metainfo, char const* url)
{
    char const* announce = nullptr;
    tr_variant* announce_list = nullptr;
    bool changed = false;
    bool const had_announce = tr_variantDictFindStr(metainfo, TR_KEY_announce, &announce, nullptr);
    bool const had_announce_list = tr_variantDictFindList(metainfo, TR_KEY_announce_list, &announce_list);

    if (!had_announce && !had_announce_list)
    {
        /* this new tracker is the only one, so add it to "announce"... */
        printf("\tAdded \"%s\" in \"announce\"\n", url);
        tr_variantDictAddStr(metainfo, TR_KEY_announce, url);
        changed = true;
    }
    else
    {
        if (!had_announce_list)
        {
            announce_list = tr_variantDictAddList(metainfo, TR_KEY_announce_list, 2);

            if (had_announce)
            {
                /* we're moving from an 'announce' to an 'announce-list',
                 * so copy the old announce URL to the list */
                tr_variant* tier = tr_variantListAddList(announce_list, 1);
                tr_variantListAddStr(tier, announce);
                changed = true;
            }
        }

        /* If the user-specified URL isn't in the announce list yet, add it */
        if (!announce_list_has_url(announce_list, url))
        {
            tr_variant* tier = tr_variantListAddList(announce_list, 1);
            tr_variantListAddStr(tier, url);
            printf("\tAdded \"%s\" to \"announce-list\" tier %zu\n", url, tr_variantListSize(announce_list));
            changed = true;
        }
    }

    return changed;
}

int tr_main(int argc, char* argv[])
{
    int changedCount = 0;

    files = tr_new0(char const*, argc);

    tr_logSetLevel(TR_LOG_ERROR);

    if (parseCommandLine(argc, (char const* const*)argv) != 0)
    {
        return EXIT_FAILURE;
    }

    if (showVersion)
    {
        fprintf(stderr, MY_NAME " " LONG_VERSION_STRING "\n");
        return EXIT_SUCCESS;
    }

    if (fileCount < 1)
    {
        fprintf(stderr, "ERROR: No torrent files specified.\n");
        tr_getopt_usage(MY_NAME, getUsage(), options);
        fprintf(stderr, "\n");
        return EXIT_FAILURE;
    }

    if (add == nullptr && deleteme == nullptr && replace[0] == 0)
    {
        fprintf(stderr, "ERROR: Must specify -a, -d or -r\n");
        tr_getopt_usage(MY_NAME, getUsage(), options);
        fprintf(stderr, "\n");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < fileCount; ++i)
    {
        tr_variant top;
        bool changed = false;
        char const* filename = files[i];
        tr_error* error = nullptr;

        printf("%s\n", filename);

        if (!tr_variantFromFile(&top, TR_VARIANT_FMT_BENC, filename, &error))
        {
            printf("\tError reading file: %s\n", error->message);
            tr_error_free(error);
            continue;
        }

        if (deleteme != nullptr)
        {
            changed |= removeURL(&top, deleteme);
        }

        if (add != nullptr)
        {
            changed = addURL(&top, add);
        }

        if (replace[0] != nullptr && replace[1] != nullptr)
        {
            changed |= replaceURL(&top, replace[0], replace[1]);
        }

        if (changed)
        {
            ++changedCount;
            tr_variantToFile(&top, TR_VARIANT_FMT_BENC, filename);
        }

        tr_variantFree(&top);
    }

    printf("Changed %d files\n", changedCount);

    tr_free((void*)files);
    return EXIT_SUCCESS;
}
