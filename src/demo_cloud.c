/**
 * @file demo_cloud.c
 * @brief Cloud content access for the interactive slave demo.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tal_api.h"
#include "http_client_interface.h"
#include "cJSON.h"

#include "demo_cloud.h"
#include "demo_network_config.h"
#include "playback_media_package.h"

#define DEMO_KV_TOKEN  "sp_cloud_token"
#define DEMO_KV_SERVER "sp_cloud_server"

#define DEMO_TOKEN_MAX   512
#define DEMO_SERVER_MAX  128
#define DEMO_AUTH_MAX    (DEMO_TOKEN_MAX + 16)
#define DEMO_HTTP_TIMEOUT_MS 15000

#ifndef DEMO_TRIGGER_AUTHORIZATION
#define DEMO_TRIGGER_AUTHORIZATION ""
#endif

static char sg_playback_auth[DEMO_AUTH_MAX];
static const char *const sg_trigger_auth_config = DEMO_TRIGGER_AUTHORIZATION;
static const char *const sg_playback_auth_config = DEMO_PLAYBACK_AUTHORIZATION;

typedef struct
{
    char host[96];
    uint16_t port;
    bool tls;
} demo_endpoint_t;

static bool demo_kv_get_string(const char *key, char *out, size_t out_max)
{
    uint8_t *val = NULL;
    size_t len = 0;

    if (tal_kv_get(key, &val, &len) != OPRT_OK || val == NULL || len == 0U) {
        if (val != NULL) {
            tal_kv_free(val);
        }
        out[0] = '\0';
        return false;
    }
    if (len >= out_max) {
        len = out_max - 1U;
    }
    memcpy(out, val, len);
    out[len] = '\0';
    tal_kv_free(val);
    /* KV values may include a trailing NUL from the writer. */
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}

static void demo_get_server(char *out, size_t out_max)
{
    /* Fixed deployment address; ignore any server URL stored during pairing. */
    snprintf(out, out_max, "%s", DEMO_VIDEO_URL);
}

static bool demo_parse_endpoint(const char *url, demo_endpoint_t *ep)
{
    const char *p = url;
    const char *host_end;
    size_t host_len;

    ep->tls = false;
    if (strncmp(p, "https://", 8) == 0) {
        ep->tls = true;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    ep->port = ep->tls ? 443U : 80U;

    host_end = strchr(p, ':');
    if (host_end != NULL) {
        ep->port = (uint16_t)atoi(host_end + 1);
    } else {
        host_end = strchr(p, '/');
        if (host_end == NULL) {
            host_end = p + strlen(p);
        }
    }
    host_len = (size_t)(host_end - p);
    if (host_len == 0U || host_len >= sizeof(ep->host)) {
        return false;
    }
    memcpy(ep->host, p, host_len);
    ep->host[host_len] = '\0';
    return true;
}

static bool demo_get_user_auth(char *out, size_t out_max)
{
    char token[DEMO_TOKEN_MAX];

    if (!demo_kv_get_string(DEMO_KV_TOKEN, token, sizeof(token))) {
        out[0] = '\0';
        return false;
    }
    snprintf(out, out_max, "Bearer %s", token);
    return true;
}

static void demo_get_trigger_auth(char *out, size_t out_max)
{
    if (sg_trigger_auth_config != NULL && sg_trigger_auth_config[0] != '\0' &&
        strstr(sg_trigger_auth_config, "replace-") == NULL) {
        snprintf(out, out_max, "%s", sg_trigger_auth_config);
        return;
    }
    (void)demo_get_user_auth(out, out_max);
}

bool demo_cloud_is_paired(void)
{
    char token[DEMO_TOKEN_MAX];

    return demo_kv_get_string(DEMO_KV_TOKEN, token, sizeof(token));
}

const char *demo_cloud_playback_authorization(void)
{
    return sg_playback_auth;
}

void demo_cloud_refresh_auth(void)
{
    if (sg_playback_auth_config != NULL && sg_playback_auth_config[0] != '\0' &&
        strstr(sg_playback_auth_config, "replace-") == NULL) {
        snprintf(sg_playback_auth, sizeof(sg_playback_auth), "%s",
                 sg_playback_auth_config);
        return;
    }
    if (!demo_get_user_auth(sg_playback_auth, sizeof(sg_playback_auth))) {
        sg_playback_auth[0] = '\0';
    }
}

/* Perform a GET and hand the parsed JSON root back to the caller. */
static demo_cloud_result_t demo_http_get_json(
    const char *path,
    const char *authorization,
    cJSON **root_out
)
{
    char server[DEMO_SERVER_MAX];
    demo_endpoint_t ep;
    http_client_response_t response;
    http_client_status_t status;
    cJSON *root;

    *root_out = NULL;
    demo_get_server(server, sizeof(server));
    if (!demo_parse_endpoint(server, &ep)) {
        return DEMO_CLOUD_BAD_RESPONSE;
    }

    http_client_header_t headers[] = {
        {.key = "Authorization", .value = authorization},
        {.key = "Accept", .value = "application/json"},
    };

    memset(&response, 0, sizeof(response));
    status = http_client_request(
        &(const http_client_request_t){
            .host = ep.host,
            .port = ep.port,
            .path = path,
            .method = "GET",
            .headers = headers,
            .headers_count = 2,
            .body = (const uint8_t *)"",
            .body_length = 0,
            .timeout_ms = DEMO_HTTP_TIMEOUT_MS,
            .tls_no_verify = ep.tls ? (DEMO_HTTP_TLS_NO_VERIFY != 0) : false,
        },
        &response);

    if (status != HTTP_CLIENT_SUCCESS) {
        PR_ERR("demo cloud: GET %s failed st=%d", path, status);
        http_client_free(&response);
        return DEMO_CLOUD_NET_ERROR;
    }
    if (response.status_code == 401 || response.status_code == 403) {
        PR_ERR("demo cloud: GET %s auth error %u", path, response.status_code);
        http_client_free(&response);
        return DEMO_CLOUD_AUTH_ERROR;
    }
    if (response.status_code != 200 || response.body == NULL) {
        PR_ERR("demo cloud: GET %s status %u", path, response.status_code);
        http_client_free(&response);
        return DEMO_CLOUD_BAD_RESPONSE;
    }

    root = cJSON_ParseWithLength((const char *)response.body,
                                 response.body_length);
    http_client_free(&response);
    if (root == NULL) {
        return DEMO_CLOUD_BAD_RESPONSE;
    }
    *root_out = root;
    return DEMO_CLOUD_OK;
}

demo_cloud_result_t demo_cloud_fetch_list(
    demo_cloud_item_t *items,
    int max_items,
    int *count_out
)
{
    char auth[DEMO_AUTH_MAX];
    cJSON *root = NULL;
    cJSON *json_items;
    demo_cloud_result_t result;
    int count = 0;

    *count_out = 0;
    if (!demo_get_user_auth(auth, sizeof(auth))) {
        return DEMO_CLOUD_NOT_PAIRED;
    }

    result = demo_http_get_json("/api/v1/contents", auth, &root);
    if (result != DEMO_CLOUD_OK) {
        return result;
    }

    json_items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(json_items)) {
        int total = cJSON_GetArraySize(json_items);
        int i;

        for (i = 0; i < total && count < max_items; i++) {
            cJSON *item = cJSON_GetArrayItem(json_items, i);
            cJSON *j_id = cJSON_GetObjectItem(item, "content_id");
            cJSON *j_label = cJSON_GetObjectItem(item, "display_label");
            cJSON *j_dur = cJSON_GetObjectItem(item, "duration_ms");
            cJSON *j_state = cJSON_GetObjectItem(item, "state");
            demo_cloud_item_t *out = &items[count];

            if (!cJSON_IsString(j_id) || j_id->valuestring == NULL) {
                continue;
            }
            memset(out, 0, sizeof(*out));
            snprintf(out->content_id, sizeof(out->content_id), "%s",
                     j_id->valuestring);
            if (cJSON_IsString(j_label) && j_label->valuestring != NULL &&
                j_label->valuestring[0] != '\0') {
                snprintf(out->label, sizeof(out->label), "%s",
                         j_label->valuestring);
            } else {
                snprintf(out->label, sizeof(out->label), "Content %d",
                         count + 1);
            }
            if (cJSON_IsNumber(j_dur)) {
                out->duration_ms = (uint32_t)j_dur->valuedouble;
            }
            out->ready = cJSON_IsString(j_state) &&
                         j_state->valuestring != NULL &&
                         strcmp(j_state->valuestring, "READY") == 0;
            count++;
        }
    }

    cJSON_Delete(root);
    *count_out = count;
    PR_NOTICE("demo cloud: fetched %d items", count);
    return DEMO_CLOUD_OK;
}

static void demo_fill_asset(
    playback_asset_t *asset,
    const cJSON *obj,
    const char *url_key,
    const char *len_key,
    const char *sha_key,
    const char *etag_key
)
{
    const cJSON *j_url = cJSON_GetObjectItem(obj, url_key);
    const cJSON *j_len = cJSON_GetObjectItem(obj, len_key);
    const cJSON *j_sha = cJSON_GetObjectItem(obj, sha_key);
    const cJSON *j_etag = cJSON_GetObjectItem(obj, etag_key);

    memset(asset, 0, sizeof(*asset));
    if (cJSON_IsString(j_url) && j_url->valuestring != NULL) {
        snprintf(asset->url, sizeof(asset->url), "%s", j_url->valuestring);
    }
    if (cJSON_IsNumber(j_len)) {
        asset->byte_length = (uint32_t)j_len->valuedouble;
    }
    if (cJSON_IsString(j_sha) && j_sha->valuestring != NULL) {
        snprintf(asset->sha256, sizeof(asset->sha256), "%s",
                 j_sha->valuestring);
    }
    if (cJSON_IsString(j_etag) && j_etag->valuestring != NULL) {
        snprintf(asset->etag, sizeof(asset->etag), "%s", j_etag->valuestring);
    }
}

/* The backend absolutizes asset URLs from the request Host header, which
 * drops the custom port. Rebase every asset URL onto DEMO_VIDEO_URL. */
static void demo_rebase_asset_url(playback_asset_t *asset)
{
    char server[DEMO_SERVER_MAX];
    const char *path;
    char rebased[sizeof(asset->url)];

    if (asset->url[0] == '\0') {
        return;
    }
    path = asset->url;
    if (strncmp(path, "https://", 8) == 0) {
        path += 8;
    } else if (strncmp(path, "http://", 7) == 0) {
        path += 7;
    } else {
        return;
    }
    path = strchr(path, '/');
    if (path == NULL) {
        return;
    }
    demo_get_server(server, sizeof(server));
    /* Drop any trailing slash on the server base. */
    {
        size_t len = strlen(server);
        if (len > 0U && server[len - 1U] == '/') {
            server[len - 1U] = '\0';
        }
    }
    snprintf(rebased, sizeof(rebased), "%s%s", server, path);
    memcpy(asset->url, rebased, sizeof(asset->url));
}

demo_cloud_result_t demo_cloud_fetch_session(
    const char *content_id,
    playback_session_t *session
)
{
    char auth[DEMO_AUTH_MAX];
    char path[PLAYBACK_CONTENT_ID_MAX_LEN + 8];
    cJSON *root = NULL;
    const cJSON *j_playback;
    const cJSON *j_video;
    const cJSON *j_audio;
    const cJSON *j_trigger;
    const cJSON *j_field;
    demo_cloud_result_t result;
    static uint32_t sg_session_counter;

    demo_get_trigger_auth(auth, sizeof(auth));
    if (auth[0] == '\0') {
        return DEMO_CLOUD_NOT_PAIRED;
    }

    snprintf(path, sizeof(path), "/c/%s", content_id);
    result = demo_http_get_json(path, auth, &root);
    if (result != DEMO_CLOUD_OK) {
        return result;
    }

    j_playback = cJSON_GetObjectItem(root, "playback");
    j_video = cJSON_GetObjectItem(j_playback, "video");
    j_audio = cJSON_GetObjectItem(j_playback, "audio");
    j_trigger = cJSON_GetObjectItem(root, "trigger");
    if (!cJSON_IsObject(j_playback) || !cJSON_IsObject(j_video) ||
        !cJSON_IsObject(j_audio)) {
        cJSON_Delete(root);
        return DEMO_CLOUD_BAD_RESPONSE;
    }

    memset(session, 0, sizeof(*session));
    sg_session_counter++;
    snprintf(session->session_id, sizeof(session->session_id),
             "demo-%08x-%u",
             (unsigned)tal_system_get_millisecond(),
             (unsigned)sg_session_counter);
    snprintf(session->content_id, sizeof(session->content_id), "%s",
             content_id);
    session->revision = 1U;

    j_field = cJSON_GetObjectItem(root, "duration_ms");
    if (cJSON_IsNumber(j_field)) {
        session->duration_ms = (uint32_t)j_field->valuedouble;
    }

    j_field = cJSON_GetObjectItem(j_playback, "profile");
    if (cJSON_IsString(j_field) && j_field->valuestring != NULL) {
        snprintf(session->profile, sizeof(session->profile), "%s",
                 j_field->valuestring);
    } else {
        snprintf(session->profile, sizeof(session->profile), "%s",
                 PLAYBACK_PROFILE_H264_MP3);
    }

    demo_fill_asset(&session->video.asset, j_video,
                    "url", "byte_length", "sha256", "etag");
    session->video.width = PLAYBACK_VIDEO_WIDTH;
    session->video.height = PLAYBACK_VIDEO_HEIGHT;
    j_field = cJSON_GetObjectItem(j_video, "width");
    if (cJSON_IsNumber(j_field)) {
        session->video.width = (uint16_t)j_field->valuedouble;
    }
    j_field = cJSON_GetObjectItem(j_video, "height");
    if (cJSON_IsNumber(j_field)) {
        session->video.height = (uint16_t)j_field->valuedouble;
    }
    session->video.fps_num = 10U;
    session->video.fps_den = 1U;
    j_field = cJSON_GetObjectItem(j_video, "fps");
    if (cJSON_IsNumber(j_field) && j_field->valuedouble > 0.0) {
        session->video.fps_num = (uint16_t)j_field->valuedouble;
    }
    session->video.max_keyframe_interval_ms = 1000U;
    j_field = cJSON_GetObjectItem(j_video, "max_keyframe_interval_ms");
    if (cJSON_IsNumber(j_field)) {
        session->video.max_keyframe_interval_ms =
            (uint16_t)j_field->valuedouble;
    }
    session->video.h264_profile_idc = PLAYBACK_H264_BASELINE_PROFILE_IDC;
    session->video.h264_level_idc = 30U;
    session->video.yuv420p = true;
    session->video.has_b_frames = false;

    demo_fill_asset(&session->audio.asset, j_audio,
                    "url", "byte_length", "sha256", "etag");
    demo_fill_asset(&session->audio.index_asset, j_audio,
                    "index_url", "index_byte_length", "index_sha256",
                    "index_etag");
    session->audio.sample_rate = PLAYBACK_MP3_SAMPLE_RATE;
    j_field = cJSON_GetObjectItem(j_audio, "sample_rate");
    if (cJSON_IsNumber(j_field)) {
        session->audio.sample_rate = (uint32_t)j_field->valuedouble;
    }
    session->audio.bitrate_kbps = PLAYBACK_MP3_BITRATE_KBPS;
    j_field = cJSON_GetObjectItem(j_audio, "bitrate");
    if (cJSON_IsNumber(j_field) && j_field->valuedouble >= 1000.0) {
        session->audio.bitrate_kbps =
            (uint16_t)(j_field->valuedouble / 1000.0);
    }
    session->audio.channels = 2U;
    j_field = cJSON_GetObjectItem(j_audio, "channels");
    if (cJSON_IsNumber(j_field)) {
        session->audio.channels = (uint8_t)j_field->valuedouble;
    }
    session->audio.index_version = PLAYBACK_AUDIO_INDEX_VERSION;
    j_field = cJSON_GetObjectItem(j_audio, "index_version");
    if (cJSON_IsNumber(j_field)) {
        session->audio.index_version = (uint8_t)j_field->valuedouble;
    }

    session->autoplay = true;
    if (cJSON_IsObject(j_trigger)) {
        j_field = cJSON_GetObjectItem(j_trigger, "autoplay");
        if (cJSON_IsBool(j_field)) {
            session->autoplay = cJSON_IsTrue(j_field);
        }
    }
    session->end_behavior = PLAYBACK_END_HOLD_LAST_FRAME;

    demo_rebase_asset_url(&session->video.asset);
    demo_rebase_asset_url(&session->audio.asset);
    demo_rebase_asset_url(&session->audio.index_asset);

    cJSON_Delete(root);
    PR_NOTICE("demo cloud: session ready content=%s dur=%u video=%s",
              session->content_id, (unsigned)session->duration_ms,
              session->video.asset.url);
    return DEMO_CLOUD_OK;
}
