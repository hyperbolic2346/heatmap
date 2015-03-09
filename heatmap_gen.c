/* heatmap_gen - Generates heatmaps for hlstatsx
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Mike Wilson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif /* __STDC_VERSION__ */
 
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <mysql.h>
#include <time.h>
#include <unistd.h>
#include <wand/MagickWand.h>

#include "heatmap.h"

#define DAYS_TO_GENERATE        (60*60*24*90)  // 90 days
#define DB_PREFIX               "hlstats"

static struct timespec diff_time(struct timespec start, struct timespec end)
{
    struct timespec temp;
    if ((end.tv_nsec-start.tv_nsec)<0) {
        temp.tv_sec = end.tv_sec-start.tv_sec-1;
        temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
    } else {
        temp.tv_sec = end.tv_sec-start.tv_sec;
        temp.tv_nsec = end.tv_nsec-start.tv_nsec;
    }
    return temp;
}

static void finish_with_error(MYSQL *con)
{
  fprintf(stderr, "%s\n", mysql_error(con));
  mysql_close(con);
  exit(1);        
}

static bool build_heatmap_for_map(MYSQL_ROW map_info, MYSQL *con, const char *web_path, const char *mode)
{
    struct timespec start_time;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start_time);
    const char *code = map_info[0];
    const char *game = map_info[1];
    const char *map = map_info[2];
    float offsetx = atof(map_info[3]);
    float offsety = atof(map_info[4]);
    unsigned int flipx = atoi(map_info[5]);
    unsigned int flipy = atoi(map_info[6]);
    unsigned int rotate = atoi(map_info[7]);
    unsigned int days = atoi(map_info[8]);
    float scale = atof(map_info[10]);
    float thumbw = atof(map_info[12]);
    float thumbh = atof(map_info[13]);
    unsigned int cropx1 = atoi(map_info[14]);
    unsigned int cropx2 = atoi(map_info[15]);
    unsigned int cropy1 = atoi(map_info[16]);
    unsigned int cropy2 = atoi(map_info[17]);

    // first thing's first...if the source image doesn't exist, there isn't much we can do
    char input_filename[4096];
    snprintf(input_filename, 4096, "./src/%s/%s.png", game, map);
    input_filename[4095] = '\0';

    if (access(input_filename, F_OK) == -1 ) {
        fprintf(stderr, "Unable to find image file '%s'. Unable to build heatmap for '%s'\n", input_filename, map);
        return false;
    }

    MagickWand *map_image = NULL;
    map_image = NewMagickWand();
    if (!MagickReadImage(map_image, input_filename)) {
        fprintf(stderr, "Unable to read image file '%s'.\n", input_filename);
        return false;
    }

    int map_image_width, map_image_height;
    map_image_width = MagickGetImageWidth(map_image);
    map_image_height = MagickGetImageHeight(map_image);

    // get data for heatmap
    char query[4096];
    snprintf(query, 4096, "SELECT 'frag' AS killtype, hef.id, hef.map, hs.game, hef.eventTime, hef.pos_x, hef.pos_y, "
                            "hef.pos_victim_x, hef.pos_victim_y FROM " DB_PREFIX "_Events_Frags as hef, "
                            DB_PREFIX "_Servers as hs WHERE (hef.map = '%s' OR hef.map = 'custom/%s') AND "
                            "hs.serverId = hef.serverId AND hs.game = '%s' AND hef.pos_x IS NOT NULL AND "
                            "hef.pos_y IS NOT NULL AND hef.eventTime >= FROM_UNIXTIME(%d) UNION ALL "
                            "SELECT 'teamkill' AS killtype, hef.id, hef.map, hs.game, hef.eventTime, hef.pos_x, "
                            "hef.pos_y, hef.pos_victim_x, hef.pos_victim_y FROM " DB_PREFIX "_Events_Teamkills as hef, "
                            DB_PREFIX "_Servers as hs WHERE (hef.map = '%s' OR hef.map = 'custom/%s') AND "
                            "hs.serverId = hef.serverId", map, map, game, (unsigned int)(time(NULL) - DAYS_TO_GENERATE), map, map);
    query[4095] = '\0';

    if (mysql_query(con, query) != 0) {
        finish_with_error(con);
    }
  
    MYSQL_RES *result = mysql_store_result(con);
  
    if (result == NULL) {
        finish_with_error(con);
    }

    unsigned long num_rows = mysql_num_rows(result);

    if (num_rows == 0) {
        printf("0 kills for map %s, skipping generation\n", map);
        mysql_free_result(result);
        return true;
    }

    MYSQL_ROW row;
  
    struct timespec hm_start_time;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &hm_start_time);

    // Create the heatmap object with the given dimensions (in pixel).
    heatmap_t* hm = heatmap_new(map_image_width, map_image_height);

    while ((row = mysql_fetch_row(result))) {
        const char *killtype = row[0];

        int posx, posy;
        if (killtype != NULL) {
            posx = atoi(row[7]);
            posy = atoi(row[8]);
        } else {
            posx = atoi(row[5]);
            posy = atoi(row[6]);
        }

        unsigned int x = abs((((offsetx * (flipx ? -1 : 1)) - posx) / scale) - 1);
        unsigned int y = abs((((offsety * (flipy ? -1 : 1)) - posy) / scale) - 1);

        if (x > map_image_width || y > map_image_height) {
            fprintf(stderr, "Got a coordinate outside bounds\n"
                            "pos_victim: %d,%d offset: %f,%f flip: %d,%d scale: %f map_dim: %d, %d\n", 
                            posx, posy, offsetx, offsety, flipx, flipy, scale, map_image_width, map_image_height);
            return false;
        }

        if (rotate) {
            heatmap_add_point(hm, y, x);
        } else {
            heatmap_add_point(hm, x, y);
        }
    }

    mysql_free_result(result);

    // generate the heatmap
    unsigned char *image = heatmap_render_default_to(hm, NULL);

    // not needed any longer
    heatmap_free(hm);

    struct timespec hm_end_time;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &hm_end_time);
    struct timespec hm_total_time;
    hm_total_time = diff_time(hm_start_time, hm_end_time);

    printf(" --- Took %lld.%.9ld nsec to generate heatmap\n", hm_total_time.tv_sec, hm_total_time.tv_nsec); 

    MagickWand *heatmap = NULL;
    MagickWandGenesis();
    
    heatmap = NewMagickWand();
    if (!MagickConstituteImage(heatmap, map_image_width, map_image_height, "RGBA", CharPixel, image)) {
        fprintf(stderr, "Unable to read heatmap rgba image!\n");
        return false;
    }

    MagickCompositeImage(map_image, heatmap, OverCompositeOp, 0, 0);

    if (cropx2 > 0 && cropy2 > 0) {
        // crop the image
        MagickCropImage(map_image, cropx2-cropx1, cropy2-cropy1, cropx1, cropy1);
    }

    // create png
    {
        struct timespec pngwrite_start_time;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &pngwrite_start_time);

        char output_image[4096];
        snprintf(output_image, 4096, "%s/hlstatsimg/games/%s/heatmaps/%s-%s.png", web_path, code, map, mode);
        output_image[4095] = '\0';

        printf("Writing heatmap %s\n", output_image);
        MagickWriteImage(map_image, output_image);
        struct timespec pngwrite_end_time;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &pngwrite_end_time);
        struct timespec pngwrite_total_time;
        pngwrite_total_time = diff_time(pngwrite_start_time, pngwrite_end_time);

        printf(" --- Took %lld.%.9ld nsec to write png\n", pngwrite_total_time.tv_sec, pngwrite_total_time.tv_nsec); 

    }

    // generate a thumbnail?
    if (thumbw > 0 && thumbh > 0) {
        size_t thumb_width = (size_t)(thumbw * (float)map_image_width);
        size_t thumb_height = (size_t)(thumbh * (float)map_image_height);
        MagickResizeImage(map_image, thumb_width, thumb_height, LanczosFilter, 1.0);

        char output_image[4096];
        snprintf(output_image, 4096, "%s/hlstatsimg/games/%s/heatmaps/%s-%s-thumb.png", web_path, code, map, mode);
        output_image[4095] = '\0';

        MagickWriteImage(map_image, output_image);
    }

    /* Clean up */
    if (heatmap != NULL) {
        heatmap = DestroyMagickWand(heatmap);
        heatmap = NULL;
    }

    if (map_image != NULL) {
        map_image = DestroyMagickWand(map_image);
        map_image = NULL;
    }
    
    MagickWandTerminus();

    free(image);

    struct timespec end_time;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end_time);
    struct timespec total_time;
    total_time = diff_time(start_time, end_time);

    printf(" --- Took %lld.%.9ld nsec to build heatmap\n", total_time.tv_sec, total_time.tv_nsec); 
    
    return true;
}

int main(int argc, char **argv)
{
    printf("found %d args\n", argc);
    if (argc != 7) {
      printf("invalid usage: %s <hostname> <port> <user> <password> <database_name> <hlstatsx_path>\n", argv[0]);
      exit(0);
    }

    MYSQL *con = mysql_init(NULL);

    if (con == NULL) {
        fprintf(stderr, "%s\n", mysql_error(con));
        exit(1);
    }

    printf("connecting\n");
    if (!mysql_real_connect(con, argv[1], argv[3], argv[4], argv[5], atoi(argv[2]), NULL, 0)) {
        printf("oh no\n");
        finish_with_error(con);
    }

    char query[1024];
    snprintf(query, 1024, "SELECT g.code, hc.game, hc.map, hc.xoffset, hc.yoffset, hc.flipx, hc.flipy, hc.rotate, hc.days, "
                            "hc.brush, hc.scale, hc.font, hc.thumbw, hc.thumbh, hc.cropx1, hc.cropx2, hc.cropy1, hc.cropy2 FROM "
                            DB_PREFIX "_Games AS g INNER JOIN " DB_PREFIX "_Heatmap_Config AS hc ON hc.game = g.realgame "
                            "WHERE game = 'insurgency' ORDER BY code ASC, game ASC, map ASC");
    query[1023] = '\0';

    printf("sending query '%s'\n", query);
    if (mysql_query(con, query) != 0) {
        finish_with_error(con);
    }
  
    printf("storing result\n");
    MYSQL_RES *result = mysql_store_result(con);
  
    if (result == NULL) {
        finish_with_error(con);
    }

    MYSQL_ROW row;
  
    while ((row = mysql_fetch_row(result))) {
        build_heatmap_for_map(row, con, argv[6], "kill");
    }
  
    mysql_free_result(result);
    mysql_close(con);

    return 0;
}
