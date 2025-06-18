// server.c
// CodeSync Server
// 컴파일: gcc server.c -o server -lws2_32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <time.h>
#include <direct.h>
#include <errno.h>

#pragma comment(lib, "ws2_32.lib")

#define BUF_SIZE 4096
#define MAX_CLNT 256
#define MAX_MSG_LEN 10240

// 클라이언트 정보 구조체
typedef struct {
    SOCKET sock;
    char id[30];
} ClientInfo;

// 함수 프로토타입 선언
unsigned WINAPI HandleClient(void* arg);
void ErrorHandling(char* msg);
int AuthenticateUser(char* id, char* pw);
void CreateProject(char* projectName, SOCKET sock);
void ProcessCommit(char* data, SOCKET sock, char* userID);
void SendHistory(char* projectName, SOCKET sock);
void RevokeCommit(char* data, SOCKET sock);
void SendCommitCode(char* data, SOCKET sock);
void BroadcastUpdate(char* projectName, SOCKET from_sock);

// 전역 변수
int clntCnt = 0;
ClientInfo clntSocks[MAX_CLNT];
HANDLE hMutex;

int main(int argc, char* argv[]) {
    WSADATA wsaData;
    SOCKET hServSock, hClntSock;
    SOCKADDR_IN servAdr, clntAdr;
    int clntAdrSz;
    HANDLE hThread;

    if (argc != 2) {
        printf("사용법: %s <포트번호>\n", argv[0]);
        exit(1);
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        ErrorHandling("WSAStartup() 오류");

    hMutex = CreateMutex(NULL, FALSE, NULL);
    hServSock = socket(PF_INET, SOCK_STREAM, 0);

    memset(&servAdr, 0, sizeof(servAdr));
    servAdr.sin_family = AF_INET;
    servAdr.sin_addr.s_addr = htonl(INADDR_ANY);
    servAdr.sin_port = htons(atoi(argv[1]));

    if (bind(hServSock, (SOCKADDR*)&servAdr, sizeof(servAdr)) == SOCKET_ERROR)
        ErrorHandling("bind() 함수 오류");
    if (listen(hServSock, 5) == SOCKET_ERROR)
        ErrorHandling("listen() 함수 오류");

    // users.txt 파일 생성 (없을 경우)
    FILE* fp = fopen("users.txt", "a");
    if (fp == NULL) {
        ErrorHandling("users.txt 파일 열기 오류");
    }
    // 기본 사용자 추가 (최초 실행 시)
    if (ftell(fp) == 0) {
        fputs("samel123 1234\n", fp);
        fputs("guest 1234\n", fp);
    }
    fclose(fp);
    printf("CodeSync 서버 시작...\n");

    while (1) {
        clntAdrSz = sizeof(clntAdr);
        hClntSock = accept(hServSock, (SOCKADDR*)&clntAdr, &clntAdrSz);
        if (hClntSock == INVALID_SOCKET) {
            ErrorHandling("accept() 오류");
            continue;
        }

        char id[30], pw[30], buffer[BUF_SIZE];
        int strLen;

        // 로그인 처리
        strLen = recv(hClntSock, buffer, BUF_SIZE - 1, 0);
        buffer[strLen] = 0;

        char* token = strtok(buffer, "\n");
        if (token) strncpy(id, token, sizeof(id) - 1);
        token = strtok(NULL, "\n");
        if (token) strncpy(pw, token, sizeof(pw) - 1);

        if (AuthenticateUser(id, pw)) {
            send(hClntSock, "LOGIN_SUCCESS##END##\n", 21, 0);

            WaitForSingleObject(hMutex, INFINITE);
            clntSocks[clntCnt].sock = hClntSock;
            strcpy(clntSocks[clntCnt].id, id);
            clntCnt++;
            ReleaseMutex(hMutex);

            hThread = (HANDLE)_beginthreadex(NULL, 0, HandleClient, (void*)&hClntSock, 0, NULL);
            printf("클라이언트 접속: %s (ID: %s)\n", inet_ntoa(clntAdr.sin_addr), id);
        } else {
            send(hClntSock, "LOGIN_FAIL##END##\n", 18, 0);
            closesocket(hClntSock);
        }
    }
    closesocket(hServSock);
    WSACleanup();
    return 0;
}

unsigned WINAPI HandleClient(void* arg) {
    SOCKET hClntSock = *((SOCKET*)arg);
    int strLen = 0;
    char msg[MAX_MSG_LEN];
    char* userID = NULL;

    WaitForSingleObject(hMutex, INFINITE);
    for (int i = 0; i < clntCnt; i++) {
        if (clntSocks[i].sock == hClntSock) {
            userID = clntSocks[i].id;
            break;
        }
    }
    ReleaseMutex(hMutex);

    if (userID == NULL) {
        closesocket(hClntSock);
        return 1;
    }

    while ((strLen = recv(hClntSock, msg, MAX_MSG_LEN -1, 0)) > 0) {
        msg[strLen] = 0;
        char* p_msg = msg;
        while( (p_msg = strstr(p_msg, "##END##")) != NULL ){
             *p_msg = '\0'; // Temporarily terminate the string
             char* command_start = msg;

             printf("명령 수신 [%s]: %.30s...\n", userID, command_start);

             char* command = strtok(command_start, "\n");
             if (!command) continue;

             char* data = command + strlen(command) + 1;

             if (strcmp(command, "create_project") == 0) {
                 CreateProject(data, hClntSock);
             } else if (strcmp(command, "commit") == 0) {
                 ProcessCommit(data, hClntSock, userID);
             } else if (strcmp(command, "history") == 0) {
                 SendHistory(data, hClntSock);
             } else if (strcmp(command, "revoke") == 0) {
                 RevokeCommit(data, hClntSock);
             } else if (strcmp(command, "view") == 0) {
                 SendCommitCode(data, hClntSock);
             } else {
                 send(hClntSock, "INVALID_COMMAND##END##\n", 23, 0);
             }

             p_msg += strlen("##END##");
             strcpy(msg, p_msg); // Move remaining buffer to the start
             p_msg = msg;
        }
    }

    // 클라이언트 연결 종료
    WaitForSingleObject(hMutex, INFINITE);
    for (int i = 0; i < clntCnt; i++) {
        if (clntSocks[i].sock == hClntSock) {
            printf("클라이언트 연결 종료: %s\n", clntSocks[i].id);
            for (int j = i; j < clntCnt - 1; j++) {
                clntSocks[j] = clntSocks[j + 1];
            }
            clntCnt--;
            break;
        }
    }
    ReleaseMutex(hMutex);
    closesocket(hClntSock);
    return 0;
}

int AuthenticateUser(char* id, char* pw) {
    FILE* fp = fopen("users.txt", "r");
    if (fp == NULL) return 0;
    char fileId[30], filePw[30];
    while (fscanf(fp, "%s %s", fileId, filePw) != EOF) {
        if (strcmp(id, fileId) == 0 && strcmp(pw, filePw) == 0) {
            fclose(fp);
            return 1; // 인증 성공
        }
    }
    fclose(fp);
    return 0; // 인증 실패
}

void CreateProject(char* projectName, SOCKET sock) {
    strtok(projectName, "\n");
    if (projectName == NULL || strlen(projectName) == 0) {
        send(sock, "PROJECT_CREATE_FAIL##END##\n", 27, 0);
        return;
    }
    if (_mkdir(projectName) == 0) {
        send(sock, "PROJECT_CREATED##END##\n", 22, 0);
    } else if (errno == EEXIST) {
        send(sock, "PROJECT_EXISTS##END##\n", 21, 0);
    } else {
        send(sock, "PROJECT_CREATE_FAIL##END##\n", 27, 0);
    }
}

void ProcessCommit(char* data, SOCKET sock, char* userID) {
    char* projectName = strtok(data, "\n");
    char* title = strtok(NULL, "\n");
    char* message = strtok(NULL, "\n");
    char* code = message + strlen(message) + 1;

    if (!projectName || !title || !message || !code) {
        send(sock, "COMMIT_FAIL##END##\n", 19, 0); return;
    }
    
    time_t commitID = time(NULL);
    char historyPath[256], snippetsPath[256];
    sprintf(historyPath, "%s/history.txt", projectName);
    sprintf(snippetsPath, "%s/snippets_%s.txt", projectName, userID);

    WaitForSingleObject(hMutex, INFINITE);
    FILE* hfp = fopen(historyPath, "a");
    if (!hfp) { ReleaseMutex(hMutex); return; }
    fprintf(hfp, "CommitID: %ld | Title: %s | Author: %s | Message: %s\n", (long)commitID, title, userID, message);
    fclose(hfp);

    FILE* sfp = fopen(snippetsPath, "a");
    if (!sfp) { ReleaseMutex(hMutex); return; }
    fprintf(sfp, "--- Commit at %ld ---\n", (long)commitID);
    fprintf(sfp, "%s\n", code);
    fclose(sfp);
    ReleaseMutex(hMutex);

    send(sock, "COMMIT_SUCCESS##END##\n", 22, 0);
    BroadcastUpdate(projectName, sock);
}

void SendHistory(char* projectName, SOCKET sock) {
    strtok(projectName, "\n");
    char historyPath[256], buffer[MAX_MSG_LEN] = {0};
    sprintf(historyPath, "%s/history.txt", projectName);

    WaitForSingleObject(hMutex, INFINITE);
    FILE* fp = fopen(historyPath, "r");
    if (!fp) {
        strcpy(buffer, "이 프로젝트에 대한 히스토리가 없습니다.");
    } else {
        fread(buffer, 1, MAX_MSG_LEN - 100, fp);
        fclose(fp);
    }
    ReleaseMutex(hMutex);

    strcat(buffer, "##END##\n");
    send(sock, buffer, strlen(buffer), 0);
}

void RevokeCommit(char* data, SOCKET sock) {
    char* projectName = strtok(data, "\n");
    char* commitIdStr = strtok(NULL, "\n");

    if (!projectName || !commitIdStr) {
        send(sock, "REVOKE_FAIL##END##\n", 20, 0); return;
    }

    char historyPath[256], tempHistoryPath[256], revokedPath[256], line[1024];
    char authorID[30] = {0}, revokedHistoryLine[1024] = {0}, revokedSnippet[BUF_SIZE] = {0};
    int found = 0;

    sprintf(historyPath, "%s/history.txt", projectName);
    sprintf(revokedPath, "%s/revoked_log.txt", projectName);
    sprintf(tempHistoryPath, "%s/history.tmp", projectName);

    WaitForSingleObject(hMutex, INFINITE);

    // 1. Find commit in history.txt, get author, and create new history
    FILE* h_orig = fopen(historyPath, "r");
    FILE* h_temp = fopen(tempHistoryPath, "w");
    if (!h_orig || !h_temp) { /* error handling */ ReleaseMutex(hMutex); return; }

    char searchStr[100];
    sprintf(searchStr, "CommitID: %s |", commitIdStr);

    while (fgets(line, sizeof(line), h_orig)) {
        if (!found && strstr(line, searchStr)) {
            found = 1;
            strcpy(revokedHistoryLine, line);
            char* p_author = strstr(line, "Author: ") + strlen("Author: ");
            char* p_author_end = strstr(p_author, " |");
            strncpy(authorID, p_author, p_author_end - p_author);
        } else {
            fputs(line, h_temp);
        }
    }
    fclose(h_orig);
    fclose(h_temp);

    if (found) {
        remove(historyPath);
        rename(tempHistoryPath, historyPath);

        // 2. Remove snippet from snippets file
        char snippetsPath[256], tempSnippetsPath[256];
        sprintf(snippetsPath, "%s/snippets_%s.txt", projectName, authorID);
        sprintf(tempSnippetsPath, "%s/snippets.tmp", projectName);
        
        FILE* s_orig = fopen(snippetsPath, "r");
        FILE* s_temp = fopen(tempSnippetsPath, "w");

        int in_block = 0;
        sprintf(searchStr, "--- Commit at %s ---", commitIdStr);
        
        while (fgets(line, sizeof(line), s_orig)) {
            if (strstr(line, searchStr)) {
                in_block = 1;
                strcat(revokedSnippet, line);
                continue;
            }
            if (in_block && strncmp(line, "--- Commit at", 13) == 0) {
                in_block = 0; // Next block started
            }
            
            if (in_block) {
                 strcat(revokedSnippet, line);
            } else {
                fputs(line, s_temp);
            }
        }
        fclose(s_orig);
        fclose(s_temp);
        remove(snippetsPath);
        rename(tempSnippetsPath, snippetsPath);

        // 3. Write to revoked_log.txt
        FILE* r_log = fopen(revokedPath, "a");
        fprintf(r_log, "[HISTORY]\n%s\n[SNIPPET]\n%s\n\n", revokedHistoryLine, revokedSnippet);
        fclose(r_log);

        send(sock, "REVOKE_SUCCESS##END##\n", 23, 0);
        BroadcastUpdate(projectName, sock);
    } else {
        remove(tempHistoryPath); // cleanup
        send(sock, "REVOKE_FAIL_NOT_FOUND##END##\n", 29, 0);
    }

    ReleaseMutex(hMutex);
}


void SendCommitCode(char* data, SOCKET sock) {
    char* projectName = strtok(data, "\n");
    char* commitIdStr = strtok(NULL, "\n");
    if (!projectName || !commitIdStr) return;

    char historyPath[256], line[1024], authorID[30] = {0};
    int found_history = 0;
    sprintf(historyPath, "%s/history.txt", projectName);

    WaitForSingleObject(hMutex, INFINITE);
    FILE* hfp = fopen(historyPath, "r");
    if (!hfp) { ReleaseMutex(hMutex); return; }

    char searchStr[100];
    sprintf(searchStr, "CommitID: %s |", commitIdStr);

    while (fgets(line, sizeof(line), hfp)) {
        if (strstr(line, searchStr)) {
            found_history = 1;
            char* p_author = strstr(line, "Author: ") + strlen("Author: ");
            char* p_author_end = strstr(p_author, " |");
            strncpy(authorID, p_author, p_author_end - p_author);
            break;
        }
    }
    fclose(hfp);

    if (found_history) {
        char snippetsPath[256], codeBuffer[BUF_SIZE] = {0};
        sprintf(snippetsPath, "%s/snippets_%s.txt", projectName, authorID);
        FILE* sfp = fopen(snippetsPath, "r");
        if(sfp){
            int in_block = 0;
            sprintf(searchStr, "--- Commit at %s ---", commitIdStr);
            while (fgets(line, sizeof(line), sfp)) {
                if (strstr(line, searchStr)) {
                    in_block = 1;
                    continue;
                }
                if (in_block && strncmp(line, "--- Commit at", 13) == 0) break;
                if (in_block) strcat(codeBuffer, line);
            }
            fclose(sfp);

            char response[MAX_MSG_LEN];
            sprintf(response, "HISTORY_CODE_RESPONSE\n%s##END##\n", codeBuffer);
            send(sock, response, strlen(response), 0);
        }
    } else {
         send(sock, "HISTORY_CODE_FAIL##END##\n", 25, 0);
    }
    ReleaseMutex(hMutex);
}


void BroadcastUpdate(char* projectName, SOCKET from_sock) {
    char msg[100];
    sprintf(msg, "UPDATE\n%s\n##END##\n", projectName);

    WaitForSingleObject(hMutex, INFINITE);
    for (int i = 0; i < clntCnt; i++) {
        if (clntSocks[i].sock != from_sock) {
            send(clntSocks[i].sock, msg, strlen(msg), 0);
        }
    }
    ReleaseMutex(hMutex);
}

void ErrorHandling(char* msg) {
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}