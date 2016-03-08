#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/param.h>
#include <pthread.h>

#define CONFIG_FILE "tcwebngin.conf"
#define DIRECTORY "root/"
#define SAVE_DIRECTORY "root"

#define CONFIG_SYMBOL_HOST "host"
#define CONFIG_SYMBOL_MAXC "max_connection"
#define DEFAULT_PORT "8888"
#define CONNECT_MAX 5                  //デフォルト最大接続数
#define CONNECT_MAXMAX 100              //最大最大接続数
#define CONNECT_MAXMIN 1                //最小最大接続数
#define FILESIZE_LENGTH 9               //ヘッダにおけるファイルサイズの桁数

#define COMMAND_UNKNOWN 0
#define COMMAND_LS 1                    //コマンド判別用マクロ
#define COMMAND_CD 2
#define COMMAND_GET 3
#define COMMAND_GETD 4
#define COMMAND_EXIT 5

#define STATUS_LENGTH 4
#define STATUS_OK 200                   //OK
#define STATUS_FORBIDDEN 403            //FORBIDDEN
#define STATUS_NOTFOUND 404             //NOTFOUND
#define STATUS_PAYLOADTOOLARGE 413      //処理できないデータ量のリクエストである
#define STATUS_SERVICE_UNAVAILABLE 503  //503エラー（接続過多)
#define STATUS_EXIT 999                 //終了サイン

#define NO_ERROR 0
#define ERROR_ARG_UNKNOWN 1001          //不明な引数
#define ERROR_ARG_TOO_MANY 1002         //引数大杉
#define ERROR_ARG_SEVERAL_MODES 1003    //指定モードが多い

#define ERROR_SOCKET 2001               //ソケット作成失敗
#define ERROR_SOCKET_OPTION 2002        //オプション設定失敗
#define ERROR_SOCKET_BIND 2003          //bind失敗
#define ERROR_SOCKET_LISTEN 2004        //listen失敗
#define ERROR_SOCKET_ACCEPT 2005        //接続受付失敗
#define ERROR_SOCKET_CLOSE 2007         //ソケットクローズ失敗

#define ERROR_CONNECT 2008              //接続エラー
#define ERROR_HOST_UNKNOWN 2009         //ホスト名が解決できない
#define CONFIG_NOTFOUND 2010            //コンフィグファイルがない
#define ERROR_RECEIVE_NAME 2010         //ファイル名が受信できない
#define ERROR_TRANSMISSION_FAILED 2011  //ファイル送信失敗
#define ERROR_OUTOFMEMORY 2012          //メモリ不足によるファイル受信失敗

#define ERROR_FILENOTFOUND 3001         //ファイルが見つからない


#define BUF_LEN 256                     //通信バッファサイズ

struct _config {
    char host[120];
    int max_connection;
};

typedef struct _config config_t;

//グローバル変数
extern config_t config;
extern config_t *cfg;
//グローバル変数ここまで


int arg_check(char* arg);
void error_message(int err);

int client_main(int debugmode);
char get_menu();
void input_a_line(char *inputline);

int client_connect(config_t *cfg);
int address_resolution(struct in_addr *servhost);

int client_receive_transmission(int connecting_socket);
int command_get_client(FILE *socketfp);
int command_getd_client(FILE *socketfp);

int transmission_command(int socketid, char *command);
int receive_status(FILE *fp, int length);
int receive_text(FILE *receive_fp);
int receive_protocol(FILE *receive_fp, int *filesize, char *filename);
int receive_filedata(FILE *fp, char **filedata, int filesize, char *filename);

int file_save(char **receive_data, int filesize, char *filename);
void dir_make(char *dir);

int config_load();
int config_param(char *configfile, char *symbolname);
int new_config();

config_t config;
config_t *cfg = &config;

////////////////////////////////////////////////////////////////////////////////////////////////////
int main(){
    int err;
    dir_make(SAVE_DIRECTORY);                   //SAVE_DIRECTORYフォルダがあるかどうか確認し、なければ作っておく
    
    while(1){
        config_load();
        err = client_connect(cfg);
        if ( err == COMMAND_EXIT ){
            break;
        }
        if ( err == 503 ){
            error_message(503);
        }
        if ( err != NO_ERROR ){
            printf("サーバに接続できませんでした\nEnterキーで再試行します\n");
            fflush(stdin);
            getc(stdin);
            continue;
        }
    }
    return 0;
}
void input_a_line(char *inputline){
    char input[256];
    char *newline;                                  //改行検出用
    
    do {
    printf("#TCGTC>>");
    fflush(stdin);
    fgets(input , 255 , stdin);
    } while (input[0] == '\n');
    
    newline = memchr(input , '\n', 255);        //fileの終端にある改行コードを検出する
    *newline = '\0';                            //'\0'に置き換える
    strncpy(inputline , input , 255);
}

int client_connect(config_t *cfg){
    struct sockaddr_in client_addr;                 //ネットワーク設定
    struct in_addr servhost;                        //サーバーのIP格納
    int port = atoi(DEFAULT_PORT);                  //ポート番号
    int ret;                                        //返り値の一時保存
    int connecting_socket = 0;                      //接続時のソケットディスクリプタ
    
    //ソケットを作成
    if ( (connecting_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        error_message(ERROR_SOCKET);
    }
    
    printf("接続します:%s\n", cfg->host );
    
    //ホスト名の解決
    if ( address_resolution(&servhost) == 0){
        return ERROR_HOST_UNKNOWN;
    }
    //アドレスファミリー・ポート番号・IPアドレス設定       設定ファイルを適用する部分
    memset(&client_addr, '\0' ,sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(port);
    client_addr.sin_addr = servhost;
    
    ret = connect( connecting_socket, (struct sockaddr *)&client_addr, sizeof(client_addr) );
    if ( ret < 0 ){
        return ERROR_CONNECT;
    }
    
    ret = client_receive_transmission(connecting_socket);
    if ( ret != NO_ERROR ){
        return ret;
    }
    
    close(connecting_socket);
    return NO_ERROR;
}

//ホスト名を解決
int address_resolution(struct in_addr *servhost){
    struct addrinfo hint;                           //getaddrinfoを使うためにaddrinfo型の構造体を宣言
    struct addrinfo *result;                        //getaddrinfoの結果を受け取る構造体
    
    memset( &hint , 0 , sizeof(struct addrinfo) );
    hint.ai_family = AF_INET;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_flags = 0;
    hint.ai_protocol = 0;
    
    if ( getaddrinfo(cfg->host, NULL, &hint, &result) != 0 ) {
        return 0;
    }
    servhost->s_addr = ((struct sockaddr_in *)(result->ai_addr))->sin_addr.s_addr;
    freeaddrinfo(result);
    return 1;
}

//接続、受送信
int client_receive_transmission(int connecting_socket){
    int ret;
    int command;
    char filename[256];                             //取得リクエストを送るファイル名
    int filesize;                                   //受け取るデータのサイズ
    FILE *socketfp;

    socketfp = fdopen(connecting_socket,"rb");            //バイナリモード読み取り専用で受信データを開く

    ret = receive_status(socketfp, STATUS_LENGTH);
    if ( ret != STATUS_OK ){
        close(connecting_socket);
        fclose(socketfp);
        return ret;
    }

    printf("サーバーに接続しました\n");
    while(1){
        do{
            command = transmission_command(connecting_socket, filename);
        } while (command == NO_ERROR);
        switch (command){
          case COMMAND_LS:
            ret = receive_status(socketfp, STATUS_LENGTH);
            if (ret == STATUS_OK){
                receive_text(socketfp);
            } else if (ret == STATUS_NOTFOUND ){
                printf("THERE IS NO FILE\n");
            }
            break;
            
          case COMMAND_CD:
            ret = receive_status(socketfp, STATUS_LENGTH);
            if ( ret == STATUS_OK ){
                printf("OK\n");
            } else if ( ret == STATUS_NOTFOUND ){
                printf("NOTFOUND 404\n");
            } else if ( ret == STATUS_FORBIDDEN ){
                printf ("FORBIDDEN 403\n");
            }
            break;
            
          case COMMAND_GET:
            command_get_client(socketfp);
            break;
            
          case COMMAND_GETD:
            command_getd_client(socketfp);
            break;
            
          case COMMAND_EXIT:
            printf("終了します\n");
            fclose(socketfp);
            return ret;
          default:
            printf("%d ERROR\n",receive_status(socketfp,STATUS_LENGTH));
            break;
        }
    }
    fclose(socketfp);
    return NO_ERROR;
}
int command_get_client(FILE *socketfp){
    int ret;
    int protocol;
    int filesize;
    char filename[256];
    char **receive_datap;                           //受け取るデータのポインタのポインタ
    char *receive_data;                             //受け取るデータのポインタ

    receive_datap = &receive_data;
    memset(filename,'\0',256);
    
    ret = receive_status(socketfp, STATUS_LENGTH);
    if (ret == STATUS_NOTFOUND){
        printf("NOTFOUND 404\n");
    } 

    while(1){
        memset(filename,'\0',256);
        protocol = receive_protocol(socketfp, &filesize, filename);
        if (protocol == DT_REG){
            ret = receive_filedata(socketfp, receive_datap, filesize, filename);
            if ( ret == NO_ERROR ){
                file_save(receive_datap, filesize, filename);
            }
        } else {
            break;
        }
    }
    return NO_ERROR;
}
int command_getd_client(FILE *socketfp){
    int ret;
    int protocol;
    int filesize;
    char filename[256];
    char **receive_datap;
    char *receive_data;
    
    receive_datap = &receive_data;
    memset(filename,'\0',256);
    
    ret = receive_status(socketfp, STATUS_LENGTH);
    if (ret == STATUS_NOTFOUND){
        printf("NOTFOUND 404\n");
    }
    while(1){
        memset(filename,'\0',256);
        protocol = receive_protocol(socketfp, &filesize, filename);
        if (protocol == DT_REG){
            ret = receive_filedata(socketfp, receive_datap, filesize, filename);
            if ( ret == NO_ERROR ){
                file_save(receive_datap, filesize, filename);
            }
        } else if (protocol == DT_DIR){
            dir_make(filename);
        } else {
            break;
        }
    }
    
    return NO_ERROR;
}

//ステータスをlength長受信
int receive_status(FILE *fp, int length){
    int index = 0;
    int readsize = 0;
    char status[length];
    char bufc;
    
    memset(status,'\0',length);
    do { 
        if ( (readsize = fread(&bufc, sizeof(char) , 1, fp)) > 0){
            status[index] = bufc;
            index ++;
        }
    } while (index != length);
    
    //受信したステータスの表示
    /*
    printf("STATUS:");
    for (int i = 0 ; i < length ; i++){
        if (status[i] == 0){printf("!");}
        else{printf("%c",status[i]);}
    }
    printf("\n");
    //*/
    
    return atoi(status);
}

//命令入力、判別、
int transmission_command(int socketid, char *command){
    int buf_len = 0;
    
    memset(command,'\0',256);
    input_a_line(command);
    
    do{
        buf_len += write( socketid , (command+buf_len) , 255 );
    } while ( command[buf_len] != '\0' );
    
   // printf("buf:%s\n",command);
    if ( strncmp(command,"ls",2) == 0 ){
        return COMMAND_LS;
    } else if ( strncmp(command,"cd ",3) == 0 ){
        return COMMAND_CD;
    } else if ( strncmp(command, "get ",4) == 0 ){
        return COMMAND_GET;
    } else if ( strncmp(command, "getd ", 5) == 0 ){
        return COMMAND_GETD;
    } else if ( strncmp(command, "exit", 4) == 0 ){
        return COMMAND_EXIT;
    } else {
        printf("不明なコマンド:%s\n",command);
        return NO_ERROR;
    }
    return NO_ERROR;
}
//受信して表示する
int receive_text(FILE *receive_fp){
    char bufc;                                                  //ソケット受信用バッファ
    int size_t;                                                 //受信したかどうかのバッファ
    int index = 0;                                              //受信した合計量
    char receive[1024];
    
    memset(receive,'\0',1024);
    
    //受信・表示
    do{
        size_t = fread(&bufc, 1, 1, receive_fp);
        if ( size_t > 0 ){
            receive[index] = bufc;
            index += size_t;
        }
    } while (bufc != '\0');
    printf("%s",receive);

    return NO_ERROR;
}
//プロトコル受信
int receive_protocol(FILE *receive_fp, int *filesize, char *filename){
    char bufc;                                                  //ソケット受信用バッファ
    int size_t;                                                 //受信したかどうかのバッファ
    int index;                                                  //受信した合計量
    char protocol[4];                                           //ファイル種別
    char filenamebuf[128];                                      //ファイル名読み込み
    char filesizebuf[16];
    
    memset(protocol,'\0',4);
    memset(filenamebuf,'\0',128);
    memset(filesizebuf,'\0',16);
    
    //ヘッダ部分読み込み(-f,-d)
    index = 0;
    do{
        size_t = fread(&bufc, 1, 1, receive_fp);
        if ( bufc == ' ' ){
            continue;
        }
        if ( size_t > 0 ){
            protocol[index] = bufc;
            index += size_t;
        } else if (size_t == 0){

        }
    } while ( (bufc != ' ') || (index == 0) );
    if ( strncmp(protocol,"-c",2) == 0 ){
        return NO_ERROR;
    }
    
    //ファイル、ディレクトリ名読み込み
    index = 0;
    do{
        size_t = fread(&bufc, 1, 1, receive_fp);
        if ( bufc == ' ' ){
            continue;
        }
        if ( size_t > 0 ){
            filenamebuf[index] = bufc;
            index += size_t;
        }
    } while (bufc != ' ');
    strncpy(filename, filenamebuf, strlen(filenamebuf));
    
 //   printf("protocol:%s\nfilename:%s\n",protocol,filename);
    if ( strncmp(protocol,"-f",2) == 0 ){
        index = 0;
        do {                                    //ファイルサイズ読み込み
            size_t = fread(&bufc, 1, 1, receive_fp);
            if ( size_t > 0 ){
                filesizebuf[index] = bufc;
                index += size_t;
            }
        } while (bufc != ' ');
        *filesize = atoi(filesizebuf);           //最後の空白は切り捨てられる
        
        return DT_REG;
    } else if ( strncmp(protocol,"-d",2) == 0){
        
        return DT_DIR;
    }
    
    return NO_ERROR;
}

//ファイル受信
int receive_filedata(FILE *receive_fp, char **receive_data, int filesize, char *filename){
    char bufc;                                                  //ソケット受信用バッファ
    int size_t;                                                 //受信したかどうかのバッファ
    int index = 0;                                              //受信した合計量

    *receive_data = (char *)calloc(filesize, sizeof(char));
    if ( !(*receive_data) ){
          printf("ファイル受信に必要なメモリがありません\n");
          return ERROR_OUTOFMEMORY;
    }
    //ファイルの受信
    do{
        if ( (size_t = fread(&bufc, 1, 1, receive_fp)) > 0 ){
           (*receive_data)[index] = bufc;
           index += size_t;
        }
    } while (index != filesize);
//    printf("%dバイト受信しました\n", filesize);

    return NO_ERROR;
}
int file_save(char **receive_data, int filesize, char *filename){
    int i = 1;                                                  //ファイル検索における通し番号管理に使用(1から始める)
    char *extensionp = memchr(filename, '.', strlen(filename)); //拡張子開始位置のポインタ
    const char dir[12] = SAVE_DIRECTORY;
    char extension[256];                                        //拡張子
    char filename_name[256];                                    //ファイル名の拡張子を除いた部分
    char filename_dir[512];                                     //ディレクトリ＋ファイル名＋(被り防止番号)＋拡張子
    FILE *save_fp;
    //変数の初期化
    memset(filename_name, '\0', 256);
    memset(extension, '\0', 256);
    memset(filename_dir,'\0',512);
    
    memcpy(extension, extensionp, strlen(filename)-(extensionp-filename));
    memcpy(filename_name, filename, extensionp-filename);
    
/*    dir_make(SAVE_DIRECTORY);                   //SAVE_DIRECTORYフォルダがあるかどうか確認し、なければ作っておく
    sprintf(filename_dir,"%s/%s",dir,filename); 
    
    save_fp = fopen(filename_dir, "r");         //とりあえずdir/filenameを開いてみる
    
    if ( save_fp != NULL ){                     //すでにdir/filenameが存在している
        do{                                     //dir/filename_name(i).extensionを開く
            fclose(save_fp);
            sprintf(filename_dir, "%s/%s(%d)%s", dir, filename_name, i, extension);
            save_fp = fopen(filename_dir, "r");
            i++;
        } while ( save_fp != NULL);
        printf("重複ファイルがありました\n");
    }
    fclose(save_fp);                            //ファイル名が確定したので読み込みモードでは閉じる
    重複回避用*/
    
    save_fp = fopen(filename,"w");
    fwrite(*receive_data, filesize, 1, save_fp);
    printf("%sに保存しました:%dbytes\n", filename, filesize);
    
    fclose(save_fp);
    free(*receive_data);
    
    return NO_ERROR;
}
//directoryフォルダの存在を判定 なければ作る
void dir_make(char *directory){
    DIR *dir;
    char path[512] = "";
    strncat(path,directory,strlen(directory));
    
    if ((dir = opendir(path)) != 0){
        closedir(dir);
    } else {
        mkdir(path,0777);
        chmod(path,0777);
        printf("%sを作成しました\n",path);
    }
}
//コンフィグを読み込む
int config_load(){
    char configfile[256];
    FILE *fp;
    char *newline;
    memset(cfg,'\0',sizeof(config_t));

    if ( (fp = fopen(CONFIG_FILE,"r")) == NULL ){   //read-mode
        fclose(fp);
        return CONFIG_NOTFOUND;                     //コンフィグがない
    }
    while( fgets(configfile, 255, fp) != NULL ){
        if ( (newline = memchr(configfile,'\n',strlen(configfile)) ) != NULL ){
            *newline = '\0';
        }
        config_param(configfile, CONFIG_SYMBOL_HOST);
    }
    cfg->max_connection = CONNECT_MAX;      //最大接続数にデフォルトの値を設定
    fclose(fp);
    return NO_ERROR;
}

//configfileのパラメータを切り出し、cfg->symbolnameに代入する
int config_param(char *configfile, char *symbolname){
    char param[256];
    int i = 0;
    int j = 0;
    memset(param,'\0',256);
    
    if ( strncmp(configfile , symbolname, strlen(symbolname)) != 0 ){//文頭がsymbolnameでない場合は失敗
        return 1;
    }
    while ( configfile[i++] != '=') {                  // = が来るまで送る 1バイト目が=でないことは前条件式で保障されている
        if ( i > strlen(configfile)){
            return 1;                                     // = がなければ失敗
        }
    }
    while ( configfile[i] == ' ') {
        i++;                                           //スベースが入っていればその分文字を進める
    }
    while ( !( (configfile[i] == '\n') || (configfile[i] == '\0')) ){
        param[j] = configfile[i];                      //\0でも\nでもない場合にコピー
        i++;
        j++;
    }
    param[j] = '\0';
    if ( !strncmp(symbolname, CONFIG_SYMBOL_HOST, strlen(symbolname)) ){//ホスト名
        memset(cfg->host,'\0',120);
        strncpy(cfg->host,param,119);                      //コンフィグ構造体にコピーする
    } else if ( !strncmp(symbolname, CONFIG_SYMBOL_MAXC, strlen(symbolname)) ){//最大接続数
        cfg->max_connection = atoi(param);
    }

    return 0;
}
//新しいコンフィグを作成する
int new_config(){
    char host[256];
    char *newline;
    FILE *fp;
    
    fp = fopen(CONFIG_FILE,"w");
    sprintf(host,"host=localhost");
    fputs(host,fp);
    fclose(fp);

    return 0;
}
//エラーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーーー
void error_message(int err){
    switch (err){				//エラーが来ていなければ素通り
      case NO_ERROR:
        break;
        
      case ERROR_ARG_UNKNOWN:
        printf("モード指定が不正です\n");
        exit(0);
        
      case ERROR_ARG_TOO_MANY:
        printf("モード指定が多すぎます\n");
        exit(0);
        
      case ERROR_ARG_SEVERAL_MODES:
        printf("モードが複数指定されています\n");
        exit(0);
        
      case ERROR_SOCKET:
        printf("ソケットの作成に失敗しました\n");
        exit(0);
        
      case ERROR_SOCKET_OPTION:
        printf("ソケットオプションの設定に失敗しました\n");
        exit(0);
        
      case ERROR_SOCKET_BIND:
        printf("ソケットのバインドに失敗しました\n");
        exit(0);
        
      case ERROR_SOCKET_ACCEPT:
        printf("ソケット通信の受付に失敗しました\n");
        exit(0);
        
      case ERROR_SOCKET_CLOSE:
        printf("ソケットのクローズに失敗しました\n");
        exit(0);
        
      case ERROR_HOST_UNKNOWN:
        printf("ホスト名を解決できませんでした\n");
        exit(0);
        
      case ERROR_CONNECT:
        printf("サーバに接続できませんでした");
        break;
        
      case ERROR_FILENOTFOUND:
        printf("ファイルがありません\n");
        break;
        
      case ERROR_TRANSMISSION_FAILED:
        printf("ファイル送信失敗\n");
        break;
        
      case 503:
        printf("503 ERROR\n");
        break;
        
      default:
        printf("不正なエラーです\nエラーコード:%d\n" , err);
        exit(0);
    }
}