// server.c
// CodeSync Server

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
#define MAX_MSG_LEN 10240 // 코드 커밋을 처리하기 위한 버퍼 크기 설정

// 클라이언트 정보를 저장하기 위한 구조체
typedef struct {
    SOCKET sock;
    char id[30];
} ClientInfo;

// 함수 선언부
unsigned WINAPI HandleClient(void* arg);                    // 클라이언트 개별 처리
void ErrorHandling(char* msg);                              // 오류 처리 함수
int AuthenticateUser(char* id, char* pw);                   // 사용자 인증
void CreateProject(char* projectName, SOCKET sock);         // 프로젝트 생성
void ProcessCommit(char* data, SOCKET sock, char* userID);  // 커밋 처리
void SendHistory(char* projectName, SOCKET sock);           // 히스토리 전송
void SendList(char* projectName, SOCKET sock);              // 목록 전송
void SearchCommits(char* projectName, char* keyword, SOCKET sock);                      // 커밋 검색
void RevokeCommitByID(char* projectName, char* commitID, char* userID, SOCKET sock);    // 커밋 무효화
void BroadcastUpdate(char* projectName, SOCKET from_sock);                              // 다른 클라이언트에게 갱신 통지

int clntCnt = 0;                     // 현재 접속 중인 클라이언트 수
ClientInfo clntSocks[MAX_CLNT];      // 클라이언트 목록
HANDLE hMutex;                       // 동기화를 위한 뮤텍스

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

    // users.txt 파일이 존재하지 않으면 생성
    FILE* fp = fopen("users.txt", "a");
    if (fp == NULL) {
        ErrorHandling("users.txt 파일 열기 오류");
    }
    // else {
    //     // 예시로 기본 사용자 계정 추가
    //     fputs("samel123 1234\n", fp);
    //     fputs("guest 1234\n", fp);
    //     fclose(fp);
    // }
    printf("CodeSync 서버 구동 중...\n");

    // 클라이언트 연결 대기 루프
    while (1) {
        clntAdrSz = sizeof(clntAdr);
        hClntSock = accept(hServSock, (SOCKADDR*)&clntAdr, &clntAdrSz);
        if (hClntSock == INVALID_SOCKET) {
            ErrorHandling("accept() error");
            continue;
        }

        char id[30], pw[30], buffer[BUF_SIZE];
        int strLen;

        // 클라이언트 로그인 처리
        strLen = recv(hClntSock, buffer, BUF_SIZE - 1, 0);
        buffer[strLen] = 0;
        
        char* token = strtok(buffer, "\n");
        if (token) strncpy(id, token, sizeof(id) - 1);
        token = strtok(NULL, "\n");
        if (token) strncpy(pw, token, sizeof(pw) - 1);
        
        if (AuthenticateUser(id, pw)) {
            send(hClntSock, "LOGIN_SUCCESS##END##\n", 21, 0);
            
            // 동기화 후 클라이언트 목록에 추가
            WaitForSingleObject(hMutex, INFINITE);
            clntSocks[clntCnt].sock = hClntSock;
            strcpy(clntSocks[clntCnt].id, id);
            clntCnt++;
            ReleaseMutex(hMutex);

            hThread = (HANDLE)_beginthreadex(NULL, 0, HandleClient, (void*)&hClntSock, 0, NULL);
            printf("클라이언트 접속 IP: %s, ID: %s\n", inet_ntoa(clntAdr.sin_addr), id);
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
    
    // 이 소켓에 해당하는 사용자 ID 찾기
    WaitForSingleObject(hMutex, INFINITE);
    for (int i = 0; i < clntCnt; i++) {
        if (clntSocks[i].sock == hClntSock) {
            userID = clntSocks[i].id;
            break;
        }
    }
    ReleaseMutex(hMutex);

    if (userID == NULL) {
        printf("오류: 소켓에 해당하는 사용자 ID를 찾을 수 없음\n");
        closesocket(hClntSock);
        return 1;
    }


    // 명령어 수신 및 처리 루프
    while ((strLen = recv(hClntSock, msg, MAX_MSG_LEN, 0)) != 0 && strLen != -1) {
        if(strstr(msg, "##END##") == NULL) continue; // 메시지가 끝까지 오지 않을 시
        
        msg[strLen] = 0;
        
        char* token_end = strstr(msg, "##END##");
        if(token_end) *token_end = '\0';
        
        printf("명령 수신 [%s]: %s\n", userID, msg);

        char* command = strtok(msg, "\n");
        if (!command) continue;

        if (strcmp(command, "create_project") == 0) {
            char* projectName = strtok(NULL, "\n");
            CreateProject(projectName, hClntSock);
        } else if (strcmp(command, "commit") == 0) {
            ProcessCommit(command + strlen(command) + 1, hClntSock, userID);
        } else if (strcmp(command, "history") == 0) {
            char* projectName = strtok(NULL, "\n");
            SendHistory(projectName, hClntSock);
        } else if (strcmp(command, "list") == 0) {
            char* projectName = strtok(NULL, "\n");
            SendList(projectName, hClntSock);
        } else if (strcmp(command, "search") == 0) {
            char* projectName = strtok(NULL, "\n");
            char* keyword = strtok(NULL, "\n");
            SearchCommits(projectName, keyword, hClntSock);
        } else if (strcmp(command, "revoke") == 0) {
            char* projectName = strtok(NULL, "\n");
            char* commitID = strtok(NULL, "\n");
            RevokeCommitByID(projectName, commitID, userID, hClntSock);
        } else {
            send(hClntSock, "INVALID_COMMAND##END##\n", 23, 0);
        }
    }

    // 클라이언트 연결 종료 처리
    WaitForSingleObject(hMutex, INFINITE);
    for (int i = 0; i < clntCnt; i++) {
        if (clntSocks[i].sock == hClntSock) {
            printf("클라이언트 연결 종료: %s\n", clntSocks[i].id);
            while (i < clntCnt - 1) {
                clntSocks[i] = clntSocks[i + 1];
                i++;
            }
            clntCnt--;
            break;
        }
    }
    ReleaseMutex(hMutex);
    closesocket(hClntSock);
    return 0;
}

// 사용자 인증
int AuthenticateUser(char* id, char* pw) {
    FILE* fp = fopen("users.txt", "r");
    if (fp == NULL) return 0;

    char fileId[30], filePw[30];
    while (fscanf(fp, "%s %s", fileId, filePw) != EOF) {
        if (strcmp(id, fileId) == 0 && strcmp(pw, filePw) == 0) {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

// 프로젝트 생성
void CreateProject(char* projectName, SOCKET sock) {
    if (projectName == NULL || strlen(projectName) == 0) {
        send(sock, "PROJECT_CREATE_FAIL##END##\n", 27, 0);
        return;
    }
    
    int result = _mkdir(projectName);
    if (result == 0) {
        send(sock, "PROJECT_CREATED##END##\n", 22, 0);
    } else if (errno == EEXIST) {
        send(sock, "PROJECT_EXISTS##END##\n", 21, 0);
    } else {
        send(sock, "PROJECT_CREATE_FAIL##END##\n", 27, 0);
    }
}

// 커밋 처리
void ProcessCommit(char* data, SOCKET sock, char* userID) {
    char* projectName = strtok(data, "\n");
    char* title = strtok(NULL, "\n");
    char* message = strtok(NULL, "\n");
    char* code = strtok(NULL, "");

    if (!projectName || !title || !message || !code) {
        send(sock, "COMMIT_FAIL##END##\n", 19, 0);
        return;
    }

    char historyPath[256], snippetsPath[256];
    sprintf(historyPath, "%s/history.txt", projectName);
    sprintf(snippetsPath, "%s/snippets_%s.txt", projectName, userID);

    WaitForSingleObject(hMutex, INFINITE);

    // history.txt 파일에 메타데이터 기록
    FILE* hfp = fopen(historyPath, "a");
    if (!hfp) {
        ReleaseMutex(hMutex);
        send(sock, "COMMIT_FAIL##END##\n", 19, 0);
        return;
    }
    
    time_t t = time(NULL);
    fprintf(hfp, "CommitID: %ld | Title: %s | Author: %s | Message: %s\n", (long)t, title, userID, message);
    fclose(hfp);

    // snippets 파일에 코드 내용 기록
    FILE* sfp = fopen(snippetsPath, "a");
    if (!sfp) {
        // Rollback history entry would be ideal here, but for simplicity, we continue
        ReleaseMutex(hMutex);
        send(sock, "COMMIT_FAIL##END##\n", 19, 0);
        return;
    }
    fprintf(sfp, "--- Commit at %ld ---\n", (long)t);
    fprintf(sfp, "Title: %s\n", title);
    fprintf(sfp, "Message: %s\n", message);
    fprintf(sfp, "Code:\n%s\n\n", code);
    fclose(sfp);

    ReleaseMutex(hMutex);
    
    send(sock, "COMMIT_SUCCESS##END##\n", 22, 0);
    
    // 다른 클라이언트에게 커밋 알림 전송
    BroadcastUpdate(projectName, sock);
}


// 프로젝트의 히스토리 내용을 클라이언트에게 전송
void SendHistory(char* projectName, SOCKET sock) {
    char historyPath[256];
    char buffer[MAX_MSG_LEN] = {0};
    sprintf(historyPath, "%s/history.txt", projectName);

    WaitForSingleObject(hMutex, INFINITE);

    FILE* fp = fopen(historyPath, "r");
    if (!fp) {
        ReleaseMutex(hMutex);
        strcat(buffer, "이 프로젝트에 대한 히스토리가 없습니다.");
    } else {
        fread(buffer, 1, MAX_MSG_LEN - 100, fp);
        fclose(fp);
    }

    ReleaseMutex(hMutex);
    
    strcat(buffer, "##END##\n");
    send(sock, buffer, strlen(buffer), 0);
}


// 커밋 무효화 기능
void RevokeCommitByID(char* projectName, char* commitID, char* userID, SOCKET sock) {
    if (!projectName || !commitID || !userID) {
        send(sock, "REVOKE_FAIL##END##\n", 20, 0);
        return;
    }

    char historyPath[256], tempHistoryPath[256];
    char snippetPath[256], tempSnippetPath[256];
    char logPath[256];

    sprintf(historyPath, "%s/history.txt", projectName);
    sprintf(tempHistoryPath, "%s/history.tmp", projectName);
    sprintf(snippetPath, "%s/snippets_%s.txt", projectName, userID);
    sprintf(tempSnippetPath, "%s/snippets_%s.tmp", projectName, userID);
    sprintf(logPath, "%s/revoked_log.txt", projectName);

    WaitForSingleObject(hMutex, INFINITE);

    FILE* hRead = fopen(historyPath, "r");
    FILE* hWrite = fopen(tempHistoryPath, "w");
    FILE* sRead = fopen(snippetPath, "r");
    FILE* sWrite = fopen(tempSnippetPath, "w");
    FILE* log = fopen(logPath, "a");

    int found = 0;
    char line[2048];
    char historyBackup[2048] = "";
    char snippetBackup[8192] = "";

    if (!hRead || !hWrite || !sRead || !sWrite || !log) {
        if (hRead) fclose(hRead);
        if (hWrite) fclose(hWrite);
        if (sRead) fclose(sRead);
        if (sWrite) fclose(sWrite);
        if (log) fclose(log);
        ReleaseMutex(hMutex);
        send(sock, "REVOKE_FAIL##END##\n", 20, 0);
        return;
    }

    // 히스토리에서 해당 CommitID 찾아서 제거 + 백업
    while (fgets(line, sizeof(line), hRead)) {
        if (!found && strstr(line, commitID)) {
            found = 1;
            strcpy(historyBackup, line);
        } else {
            fputs(line, hWrite);
        }
    }

    fclose(hRead);
    fclose(hWrite);

    // 스니펫에서 해당 CommitID 포함된 블록 찾아 제거 + 백업
    int inTargetBlock = 0;
    int captured = 0;
    rewind(sRead);
    while (fgets(line, sizeof(line), sRead)) {
        if (strstr(line, commitID) && strstr(line, "--- Commit at ")) {
            inTargetBlock = 1;
            strcat(snippetBackup, line);
            captured = 1;
            continue;
        }

        if (inTargetBlock) {
            strcat(snippetBackup, line);
            if (strcmp(line, "\n") == 0) {
                inTargetBlock = 0;
            }
            continue;
        }

        fputs(line, sWrite);
    }

    fclose(sRead);
    fclose(sWrite);

    // 삭제한 내용 로그에 백업
    if (captured && found) {
        fprintf(log, "[HISTORY] %s", historyBackup);
        fprintf(log, "[SNIPPET]\n%s\n", snippetBackup);
    }

    fclose(log);

    // 파일 교체
    remove(historyPath);
    rename(tempHistoryPath, historyPath);
    remove(snippetPath);
    rename(tempSnippetPath, snippetPath);

    ReleaseMutex(hMutex);

    if (captured && found) {
        send(sock, "REVOKE_SUCCESS##END##\n", 23, 0);
        BroadcastUpdate(projectName, sock);
    } else {
        send(sock, "REVOKE_FAIL_NOT_FOUND##END##\n", 29, 0);
    }
}


// 프로젝트 변경 사항을 다른 클라이언트들에게 알림
void BroadcastUpdate(char* projectName, SOCKET from_sock) {
    char msg[100];
    sprintf(msg, "UPDATE\n%s\n##END##\n", projectName);
    
    WaitForSingleObject(hMutex, INFINITE);
    for(int i = 0; i < clntCnt; i++) {
        // 원래 커밋한 클라이언트를 제외한 나머지에게만 전송
        if (clntSocks[i].sock != from_sock) {
            send(clntSocks[i].sock, msg, strlen(msg), 0);
        }
    }
    ReleaseMutex(hMutex);
}


// 커밋 리스트 전송 (간소화된 구현: history 전체를 전송)
void SendList(char* projectName, SOCKET sock) {
    // 실제로는 제목만 추출하는 것이 좋았겠지만, 단순화를 위해 history 전체 전송
    SendHistory(projectName, sock);
}
// 커밋 검색 (클라이언트 측에서 검색하도록 유도)
void SearchCommits(char* projectName, char* keyword, SOCKET sock) {
    // 계획대로의 구현이라면 검색 결과만 추출해야 하지만, 단순화를 위해 history 전체 전송
    SendHistory(projectName, sock);
}


void ErrorHandling(char* msg) {
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}