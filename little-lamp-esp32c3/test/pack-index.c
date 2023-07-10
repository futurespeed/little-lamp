#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

uint8_t buf[10000];
uint32_t buf_len;

int main(int argc, char const *argv[])
{
    char *zip_file_name = "./index.html.gzip";
    char *dist_file_name = "../src/index_html.h";

    //获取文件大小
    struct stat stat_buf;
    stat(zip_file_name, &stat_buf);
    buf_len = stat_buf.st_size;
    printf("len: %d\r\n", buf_len);
    
    
    //读入缓冲
    FILE *rf = fopen(zip_file_name, "rb");
    fread(buf, buf_len, 1, rf);
    fclose(rf);

    //写文件
    FILE *f = fopen(dist_file_name, "w");
    char tmp_str[100];
    sprintf(tmp_str, "\r\n//File: index.html.tar.gz, Size: %d\r\n\r\n#define index_html_gz_len %d\r\n", buf_len, buf_len);
    fputs(tmp_str, f);
    fputs("const uint8_t index_html_gz[] = {\r\n", f);
    int line_cnt = 0;
    for(int i = 0; i < buf_len; i++)
    {
        sprintf(tmp_str, "0x%x", buf[i]);
        fputs(tmp_str, f);
        if(i != buf_len - 1)
        {
            fputs(",", f);
        }
        line_cnt++;
        if(line_cnt >=16){
            fputs("\r\n", f);
            line_cnt = 0;
        }
    }
    fputs("\r\n};", f);
    fclose(f);
    return 0;
}