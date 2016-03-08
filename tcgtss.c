/*
サーバープログラム
*/

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
#define FILENAME_KARI "root/tcusrs-00.csv"
#define SAVE_DIRECTORY "local"

#define CONFIG_SYMBOL_HOST "host"
#define CONFIG_SYMBOL_MAXC "max_connection"
#define DEFAULT_PORT "8888"
#define CONNECT_MAX 5                   //デフォルト最大接続数
#define FILESIZE_LENGTH 9               //ヘッダにおけるファイルサイズの桁数

#define COMMAND_UNKNOWN 0
#define COMMAND_LS 1                    //コマンド判別用マクロ
#define COMMAND_CD 2
#define COMMAND_GET 3
#define COMMAND_GETD 4
#define COMMAND_EXIT 5

#define STATUS_LENGTH 4
#define STATUS_OK 200                   //OK
#define STATUS_BADREQUEST 400           //命令がおかしい
#define STATUS_FORBIDDEN 403            //アクセス拒否
#define STATUS_PAYLOADTOOLARGE 413      //処理できないデータ量のリクエストである
#define STATUS_NOTFOUND 404             //NOTFOUND
#define STATUS_SERVICE_UNAVAILABLE 503  //503エラー（接続過多)

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

#define CLIENT_GET '1'                  //クライアントモード ファイル取得
#define CLIENT_CONFIG '2'               //設定
#define CLIENT_EXIT '9'                 //終了


struct _config {
    char host[120];
    int max_connection;
};

struct _threadinfo{
    int socket;
    int state;
};

struct _files {
    char f_name[128];
    int f_type;
};

typedef struct _config config_t;
typedef struct _threadinfo threadinfo_t;

void error_message(int err);
int server_main();
int server_setup(int *listening_socket);
void connection_number();
void connect_thread(threadinfo_t *thread);

int command_interpret(int socketid, char *current_dir);
int list_get(char *current_dir, struct _files *dirlist);
int command_ls(int socketid, struct _files *dirlist);
int command_cd(int socketid, struct _files *dirlist, char *current_dir, char *argvalue);
int exist_check(struct _files *dirlist, struct _files *loadlist, char *current_dir, char *argvalue);
int command_get(int socketid, struct _files *dirlist, char *current_dir, char *argvalue);
int struct_cmp(struct _files *dirlist, char *compared, int mode);
int command_getd(int socketid, struct _files *dirlist, char *current_dir, char *argvalue);

int receive_command(int socketid, char *filename);
int load_filedata(char *filename, char *current_dir, char **filedata, int *filesize);
int transmission_fileprotocol(int socketid, char *filename, int filesize);
int transmission_filedata(int socketid, char **filedata, int filesize);
int transmission_dirdata(int socketid, char *dirname);
int transmission_close(int socketid);
int status_send(int socketid, int status, int length);
int receive_status(int socketid, int length);

threadinfo_t thread_global[CONNECT_MAX+1]; 

//接続状態を監視しつつ待機し、接続があれば新しい子スレッドを作る。
int main(){
     int i;
     int err;
     int port = atoi(DEFAULT_PORT);
     int listening_socket;
     pthread_t thread_id[CONNECT_MAX+1];    //スレッド識別変数(最後の一つはエラー用)
     struct sockaddr_in peer_sin;
     socklen_t len = sizeof(struct sockaddr_in);
     
     server_setup(&listening_socket);
     printf("使用ポート:%d\n", port);
     while(1){
          for (i = 0; i < CONNECT_MAX+1 ; i++){
               if (thread_global[i].state != -1){
                    continue;
               }//以降 thread_global[i].state == -1 がTRUEのときの処理
               //ステータスがヒマだった通信用ソケットを使用して接続待機
               thread_global[i].state = 0;                                      //スレッドのステータスを接続待ちに変える
               connection_number();
               thread_global[i].socket = accept( listening_socket , (struct sockaddr *)&peer_sin , &len );
               if ( thread_global[i].socket == -1){
                    error_message(ERROR_SOCKET_ACCEPT);
               }
               if ( i < CONNECT_MAX){                                 //max_connectionを超えていない
                    status_send(thread_global[i].socket, STATUS_OK, STATUS_LENGTH);  //ヘッダを送る:000(エラーなし)
                    printf("接続しました:%s\n",inet_ntoa(peer_sin.sin_addr));   //クライアントのIPアドレスを表示
                    thread_global[i].state = 1;
                    pthread_create(&thread_id[i], NULL, (void *)connect_thread, &thread_global[i]); //スレッドを作る
                    pthread_detach(thread_id[i]);                                            //スレッドの終了は待たない
                    break;
               } else {                                                       //max_connectionを超えている
                    status_send(thread_global[i].socket, STATUS_SERVICE_UNAVAILABLE, STATUS_LENGTH);//ヘッダを送る:503(アクセス過多)
                    printf("接続過多により接続を拒否します:%s\n",inet_ntoa(peer_sin.sin_addr));
                    thread_global[i].state = -1;                                     //ステータスをヒマに戻す
                    err = close(thread_global[i].socket);                            //通信用ソケットを破棄
                    if (err == -1){
                         error_message(ERROR_SOCKET_CLOSE);
                    }
               }
          }//for
     }//while
     
     err = close(listening_socket);
     if (err == -1){
          error_message(ERROR_SOCKET_CLOSE);
     }
     return 0;
}
//サーバーの初期設定
int server_setup(int *listening_socket){
     int err;
     int i;
     int sock_optival = 1;
     struct addrinfo hints;                         //getaddrinfoに渡すデータ
     struct addrinfo *result;                       //getaddrinfoから受け取るデータ
     struct addrinfo *rp;
     
     for (i=0 ; i < CONNECT_MAX+1 ; i++){
          thread_global[i].state = -1;              //初期化
     }
     memset(&hints,0,sizeof(hints));
     hints.ai_family = AF_INET;
     hints.ai_socktype = SOCK_STREAM;
     hints.ai_flags = AI_PASSIVE;
     hints.ai_protocol = 0;
     
     err = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result);
     if (err != 0){
          printf("getaddrinfo FAILED\n");
          exit(0);
     }
     rp = result;                                   //getaddrinfoで受け取ったアドレス構造体の配列
     for (rp = result; rp != NULL; rp = rp->ai_next) { //構造体ごとにバインドを試す
          *listening_socket = socket( rp->ai_family, rp->ai_socktype, rp->ai_protocol );
          if (*listening_socket == -1){             //ソケットが作れなかったらスルー
               printf("a\n");
               continue;
          }
          setsockopt(*listening_socket, SOL_SOCKET, SO_REUSEADDR, &sock_optival, sizeof(sock_optival) );
          if (bind(*listening_socket, rp->ai_addr, rp->ai_addrlen) == 0){
            break;                                  //バインド成功
          }
          close(*listening_socket);                 //バインドに失敗したのでソケットを閉じる
     }
     if (rp == NULL) {                              //有効なアドレス構造体がなかった
          error_message(ERROR_SOCKET_BIND);
     }
     freeaddrinfo(result);
     //ポートを見張るようにOSに命令する
     err = listen ( *listening_socket, SOMAXCONN) ;
     if ( err == -1 ){
          error_message(ERROR_SOCKET_LISTEN);
     }
     return 0;
}
//接続数を表示 / global変数のthreadを参照しています
void connection_number(){
     int state_check;
     //thread_global[i].state(1:接続中 0:接続待機 -1:ヒマ)
     state_check = 0;
     for (int i = 0 ; i < CONNECT_MAX+1 ; i++){
          if (thread_global[i].state == 1 ){
               state_check ++;
          }
//   printf("%d.",thread_global[i].state);
     }
     printf("接続数:(%d/%d)\n", state_check, CONNECT_MAX);
}

//子スレッドでの処理
void connect_thread(threadinfo_t *thread){
    int err;
    int status;
    char current_dir[256] = DIRECTORY; 
    
    while (1){
        err = command_interpret(thread->socket, current_dir);      //データのやりとり
        if (err == COMMAND_EXIT){
            break;
        } else if (err !=NO_ERROR){
            printf("エラーが発生したのでスレッドを破棄します\n");
            break;                                       //クライアントが落ち
        }
    }
    thread->state = -1;                               //スレッドのステータスを解放済みに変える
    printf("通信を終了しました\n");
    err = close(thread->socket);
    if (err == -1){
        error_message(ERROR_SOCKET_CLOSE);
    }
    connection_number();
}

//クライアントの命令を解釈し、各命令を実行する
int command_interpret(int socketid, char *current_dir){
    int err;
    char argvalue[256];
    struct _files dirlist[30];
    
    memset(argvalue, '\0', 256);
    memset(dirlist, 0, sizeof(struct _files)*30);
    list_get(current_dir, dirlist);
    
    err = receive_command(socketid, argvalue);
    if (err != NO_ERROR){
        return err;
    }
    printf("current:%s\n",current_dir);
    
    if ( strncmp(argvalue,"ls",2) == 0 ){
        command_ls(socketid, dirlist);
    } else if ( strncmp(argvalue,"cd ",3) == 0 ){
        command_cd(socketid, dirlist, current_dir, argvalue+3);
    } else if ( strncmp(argvalue, "get ",4) == 0 ){
        command_get(socketid, dirlist, current_dir, argvalue+4);
        transmission_close(socketid);
    } else if ( strncmp(argvalue, "getd ", 5) == 0 ){
        command_getd(socketid, dirlist, current_dir, argvalue+5);
        transmission_close(socketid);
    } else if ( strncmp(argvalue, "exit", 4) == 0 ){
        printf("exit\n");
        return COMMAND_EXIT;
    } else {
        printf("不明なコマンド:%s\n",argvalue);
    }

    return NO_ERROR;
}
//命令の受信
int receive_command(int socketid, char *argvalue){
    int readsize;
    int index = 0;
    int length;
    char buf[256];
    char bufc = '\0';
    FILE *socketfp;
    
    memset(buf,'\0',256);
    socketfp = fdopen(socketid, "r");
    fseek(socketfp, 0, SEEK_END);
    length = ftell(socketfp);
    fseek(socketfp, 0, SEEK_SET);
    
    do { 
        if ( (readsize = fread(&bufc, 1 , 1, socketfp)) > 0){
            buf[index] = bufc;
            index ++;
        }
    } while (bufc != '\0');
    
    if ( buf[0] == '\0' ){
        printf("命令の受信エラーです\n");
        return ERROR_RECEIVE_NAME;
    }
    printf("受信しました>>%s\n",buf);
    strcpy(argvalue, buf);
    
    return NO_ERROR;
}

//ステータスの送信 length長に右詰め0埋めフォーマットに変換して送信
int status_send(int socketid, int status, int length){
    int digit = 0;              //桁数
    int i;                      //文字列に変換する時に使うカウント変数
    int dig_status = status;    //statusの桁数を調べるためのコピー
    char *status_send;          //送られる文字列
    char *status_char;          //文字列に変換したステータス
    
    int index = 0;              //現在書き込んだ文字数
    int writesize = 0;          //書き込みサイズ
    status_send = (char *)calloc(length, sizeof(char)); //length長で0埋め
    status_char = (char *)calloc(length, sizeof(char));
    
    for (digit=0;dig_status!=0;digit++){
        dig_status = dig_status / 10;
    }
    if (digit > length){
        printf("BAD STATUS\n");
        return 1;               //桁数がフォーマット長より大きい場合はエラー
    }
    
    sprintf(status_char,"%d",status);     //status_charにsprintfで形式変換する
    for (i=0 ; i < digit ; i++){          //status_sendに送る形式で設定する
        status_send[digit-i-1] = status_char[digit-i-1];     //statusの一桁目から詰める
    }
    do{
        if ( (writesize = write(socketid, status_send+index , 1)) > 0){
        index++;
        }
    } while (index != length);
    //送信ステータスの表示
    /*     printf("HEADER:");
    for (int i = 0;i<length;i++){
    if (status_send[i] == 0){printf("!");}
    else{printf("%c",status_send[i]);}
    }printf("\n");*/
    
    free(status_send);
    free(status_char);
    return 0;
}

//ステータスをlength長受信
int receive_status(int socketid, int length){
    int index = 0;
    int readsize = 0;
    char status[length];
    char bufc;
    FILE *fp;
    
    fp = fdopen(socketid,"rb");
    memset(status,'\0',length);
    do { 
        if ( (readsize = fread(&bufc, sizeof(char) , 1, fp)) > 0){
            status[index] = bufc;
            index ++;
        }
    } while (index != length);
    
    //受信したステータスの表示
/*    printf("STATUS:");
    for (int i = 0 ; i < length ; i++){
        if (status[i] == 0){printf("!");}
        else{printf("%c",status[i]);}
    }
    printf("\n");*/
    fclose(fp);
    return atoi(status);
}

//ファイルリストを読み込んでstruct _filesに読み込み
//f_name:ファイル・フォルダ名 f_type:ファイルタイプ
int list_get(char *current_dir, struct _files *dirlist){
    int i;
    char str[256] = "ls";
    struct dirent *dp;
    DIR *dir;
    
    memset(dirlist,0,sizeof(struct _files)*30);
    dir = opendir(current_dir);
    
    i = 0;
    for(dp = readdir(dir); dp != NULL ; dp = readdir(dir)){
        if (dp->d_name[0] == '.'){
            continue;
        }
        strncat(dirlist[i].f_name,dp->d_name,strlen(dp->d_name));
        dirlist[i].f_type = dp->d_type;
        i++;
    }
    closedir(dir);
    return 0;
}

//lsコマンドが入力されていた場合の処理
//ファイルリストを受け取って改行区切りでchar化、成功ヘッダを乗せて送信
int command_ls(int socketid, struct _files *dirlist){
    int i;
    int index = 0;
    int writesize = 0;
    char sendchar[1024];
    memset(sendchar,0,1024);
    
    
    
    //フォルダが空
    if (dirlist[0].f_name[0] == '\0'){
        printf("There is NO FILE\n");
        status_send(socketid, STATUS_NOTFOUND, STATUS_LENGTH);
        return NO_ERROR;
    }
    
    //フォルダが空でない
    status_send(socketid, STATUS_OK, STATUS_LENGTH);    //200
    i = 0;
    do {                                        //ディレクトリを先に並べる
        if (dirlist[i].f_type == DT_DIR){
            strncat(sendchar,dirlist[i].f_name,strlen(dirlist[i].f_name));
            strncat(sendchar,"\n",strlen("\n"));
        }
        i++;
    } while (dirlist[i].f_name[0] != '\0');
    i = 0;
    do {                                        //ファイル
        if (dirlist[i].f_type == DT_REG){
            strncat(sendchar,dirlist[i].f_name,strlen(dirlist[i].f_name));
            strncat(sendchar,"\n",strlen("\n"));
        }
        i++;
    } while (dirlist[i].f_name[0] != '\0');
    
    //                                          送信
    do{
        writesize = write(socketid, sendchar+index, 1);
        if (writesize == -1){
            return ERROR_TRANSMISSION_FAILED;
        }
        index += writesize;
    } while (index != strlen(sendchar)+1);
    
    return NO_ERROR;
}

//cdコマンドが入力されていた場合の処理
//~と..だけ別に処理する
int command_cd(int socketid, struct _files *dirlist, char *current_dir, char *argvalue){
    int i = 0;
    char *cut;

    if ( argvalue[0] == '~' ){                      //rootディレクトリに戻る
        strcpy(current_dir, DIRECTORY);

    } else if ( strcmp(argvalue,"..") == 0){        //一つ戻る
        if ( strlen(current_dir) == strlen(DIRECTORY) ){  //DIRECTORYの文字数とカレントの文字数が同じ
            printf("ACCESS FORBIDDEN\n");
            status_send(socketid, STATUS_FORBIDDEN, STATUS_LENGTH);
            return NO_ERROR;
        }
        current_dir[strlen(current_dir)-1] = '\0';          //最後のスラッシュを削る
        cut = strrchr(current_dir, '/');                    //二つ手前のスラッシュのポインタを取得
        cut[1] = '\0';                                      //スラッシュの次の要素を0にする(続きを切り捨て)

    } else {                                        //普通の引数
        if ( struct_cmp(dirlist, argvalue, DT_DIR) ){
            strncat(current_dir, argvalue, strlen(argvalue));//フォルダとスラッシュを足す
            strncat(current_dir, "/", 1);
        } else {
            status_send(socketid, STATUS_NOTFOUND, STATUS_LENGTH);
            return NO_ERROR;
        }
    }////////////////////////////////////////////////////////////////////404コードが入ってない/////////////////////
    status_send(socketid, STATUS_OK, STATUS_LENGTH);
    list_get(current_dir, dirlist);
    printf("currentdir:%s\n",current_dir);
    return NO_ERROR;
}

//getコマンドが入力されていた場合の処理
//構造体内をチェック(exist_check)したのちに読み込みを行う
//読み込みエラーが発生した場合はバッファを解放して戻る
int command_get(int socketid, struct _files *dirlist, char *current_dir, char *argvalue){
    int i = 0;
    int filesize = 0;
    int status;
    char *filedata;
    char **filedatap;
    char filepath[512];                         //ファイルのパス
    struct _files loadlist[30];                 //読み込むファイルのリスト
    
    memset(loadlist, '\0', sizeof(struct _files)*30);
    
    filedatap = &filedata;                      //load_filedataでmalloc transmission_filedataでfree
    if ( !exist_check(dirlist, loadlist, current_dir, argvalue) ){
        status_send(socketid, STATUS_NOTFOUND, STATUS_LENGTH);
        printf("404\n");
        return NO_ERROR;
    } else {
        printf("200\n");
        status_send(socketid, STATUS_OK, STATUS_LENGTH);
    }
    //exist_checkがTRUEならば読み込ませつつ送信する(通信機構と組み合わせる際に作る) fopen失敗ならば途中でやめる
    while (loadlist[i].f_name[0] != 0) {
        status = load_filedata(loadlist[i].f_name, current_dir, filedatap, &filesize);
        //      printf("status:%d\n",status);
        sprintf(filepath, "%s%s", current_dir, loadlist[i].f_name);
        if (status == STATUS_OK){
            transmission_fileprotocol(socketid,filepath, filesize);
            transmission_filedata(socketid, filedatap, filesize);
        }
        printf("send:%s\n",loadlist[i].f_name);
        i++;
    }
    return NO_ERROR;
}
//ファイル一覧と要求ファイルを比較してファイルの有無をチェックする あったものは構造体に格納して返す
int exist_check(struct _files *dirlist, struct _files *loadlist, char *current_dir, char *argvalue){
    int i = 0;
    int j = 0;
    char *front = argvalue;
    char *back = argvalue;
    char buf[128];
    
    //1ブロックずつ取り出して構造体の内容と比較する
    while (1){
        front = strchr(back, ' ');              //' 'を探す
        if (front == NULL){                     //最後のブロック
            strcpy(buf, back);
            if (buf[0] == '\0'){
                break;
            }
        } else {
            strncpy(buf, back , front - back );
            back = front + 1;
        }
        if ( struct_cmp(dirlist, buf, DT_REG) == 0 ){   //一致するものがない
            return 0;
        } else {                                        //一致するものがあった
            strcpy(loadlist[j].f_name, buf);
            j++;
        }
        if (front == NULL){
            break;
        }
    }
    //全て見つかった
    return 1;
}
//構造体配列でf_typeがDT_REGのもののf_nameと比較して真偽値を返す
//いずれかに一致すればTRUE 一致するものがなければFALSE
//modeで探索対象(ファイル、フォルダ)を指定する
int struct_cmp(struct _files *dirlist, char *compared, int mode){
    int i = 0;
    
    while ( dirlist[i].f_name[0] != '\0' ){
        if (dirlist[i].f_type == mode){
            if ( strcmp(dirlist[i].f_name, compared) == 0 ){
                return 1;
                }
        }
        i++;
    }
    return 0;
}

//getdが入力された
int command_getd(int socketid, struct _files *dirlist, char *current_dir, char *argvalue){
    int i = 0;
    int status;
    int filesize;
    char *filedata;
    char **filedatap;
    char path[512];                              //ディレクトリを変更するため新しいバッファ
    char path_d[512];                            //再帰処理のためのディレクトリバッファ
    char filepath[512];                          //ファイル名まで入ったパス
    struct _files dirlist_getd[30];
    
    filedatap = &filedata;                     //load_filedataでmalloc transmission_filedataでfree
    
    if ( argvalue[0] == '\0' ){                 //argvalueで空文字指定
        strcpy(path, current_dir);
//        printf("directory:%s\n",dir);
    } else {
        if ( struct_cmp(dirlist, argvalue, DT_DIR) == 0 ){
            printf("404\n");
            status_send(socketid, STATUS_NOTFOUND, STATUS_LENGTH);
            return 1;
        } else {
            status_send(socketid, STATUS_OK, STATUS_LENGTH);
            sprintf(path, "%s%s/", current_dir, argvalue);
            transmission_dirdata(socketid, path);
        }
    }
    list_get(path,dirlist_getd);
    
    while ( dirlist_getd[i].f_name[0] != '\0' ){
        if (dirlist_getd[i].f_type == DT_DIR){  //対象がディレクトリ
            sprintf(path_d, "%s%s/", path, dirlist_getd[i].f_name);
            //ディレクトリ情報を送信
            transmission_dirdata(socketid, path_d);
            command_getd(socketid, dirlist_getd, path_d, "");
        }
        if (dirlist_getd[i].f_type == DT_REG){
            //ファイル読み込み、送信
            status = load_filedata(dirlist_getd[i].f_name, path, filedatap, &filesize);
            //           printf("status:%d\n",status);
            sprintf(filepath, "%s%s", path, dirlist_getd[i].f_name);
            if (status == STATUS_OK){
                transmission_fileprotocol(socketid, filepath, filesize);
                transmission_filedata(socketid, filedatap, filesize);
            }
        }
        i++;
    }
    return 1;
}

//ファイルの読み込み
//callocでバッファ確保しているのでtransmission_filedataと組で使うこと
int load_filedata(char *filename, char *current_dir, char **filedata, int *filesize){
    char buf[256];
    char path[512];
    sprintf(path, "%s%s", current_dir, filename);
    
    FILE *fp;
//    printf("path:%s\n",path);
    fp = fopen(path, "r");
    if (fp == NULL){
        return STATUS_NOTFOUND;
    }
    fseek(fp, 0, SEEK_END);
    *filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
//    printf("filesize:%d\n", *filesize);
    *filedata = (char *)calloc(*filesize, sizeof(char));
    if ( !(*filedata) ){
        printf("PAY LOAD TOO LARGE");
        return STATUS_PAYLOADTOOLARGE;
    }
    fread(*filedata, sizeof(char), *filesize, fp);   //filedataにfilesize分ファイルを読み込む
    fclose(fp);
    return STATUS_OK;
}
//ファイルプロトコルの送信
//ここで送るのはfilesizeの次のスペースまで
//プロトコル: -f filename filesize filedata
int transmission_fileprotocol(int socketid, char *filename, int filesize){
    int index = 0;
    int writesize = 0;
    char protocol[128];         //送信プロトコル
    sprintf(protocol,"-f %s %d ", filename, filesize);
    printf("%sfff\n",protocol);
     do{
        writesize = write(socketid, protocol+index, 1);
        if (writesize == -1){
            return ERROR_TRANSMISSION_FAILED;
        }
        index += writesize;
    } while (index != strlen(protocol));
    
    return NO_ERROR;
}

//ファイルの送信
//filedataはここで解放する
//プロトコルのfiledata部分のみ送信: -f filename filesize filedata 
int transmission_filedata(int socketid, char **filedata, int filesize){
    int index = 0;              //現在書き込んだ文字数
    int writesize = 0;          //書き込みサイズ

    do{
        writesize = write(socketid, *filedata+index , 1);
        if ( writesize == -1 ){
            free(*filedata);
            return ERROR_TRANSMISSION_FAILED;
        }
        index += writesize;
    } while (index != filesize);
    index = 0;
    do{
        writesize = write(socketid, " ", 1);
        if ( writesize == -1 ){
            return ERROR_TRANSMISSION_FAILED;
        }
        index += writesize;
    } while (index != 1);
    free(*filedata);
    return NO_ERROR;
}
//ディレクトリ情報の送信 プロトコルもここで送信している
int transmission_dirdata(int socketid, char *dirname){
    int index = 0;
    int writesize = 0;
    char protocol[128];
    sprintf(protocol,"-d %s ",dirname);
    
    do{
        writesize = write(socketid, protocol+index, 1);
        if (writesize == -1){
            return ERROR_TRANSMISSION_FAILED;
        }
        index += writesize;
    } while (index != strlen(protocol));
    printf("%sddd\n",protocol);
    return NO_ERROR;
}
int transmission_close(int socketid){
    int index = 0;
    int writesize = 0;
    char protocol[128] = "-c ";                 //スペースを検知しているので最後のスペースが重要
    
    do{
        writesize = write(socketid, protocol+index, 1);
        if (writesize == -1){
            return ERROR_TRANSMISSION_FAILED;
        }
        index += writesize;
    } while (index != strlen(protocol));
    printf("%sccc\n",protocol);
    return NO_ERROR;
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