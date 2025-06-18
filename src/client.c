// client.c
// CodeSync Client
// 명세 기반 구현 (수신 스레드 분리)
// 컴파일: gcc client.c -o client -lws2_32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

#define BUF_SIZE 4096
#define MAX_MSG_LEN 10240

// 연결 리스트 구조체 
typedef struct CommitNode {
    char data[1024];
    struct CommitNode* next;
} CommitNode;

// 함수 프로토타입
unsigned WINAPI RecvMsg(void* arg);
void ErrorHandling(char* msg);
void DisplayHistory(char* historyData);
void FreeHistory(CommitNode* head);

// 전역 변수
char name[30] = "[DEFAULT]";
char msg[MAX_MSG_LEN];
char currentProject[50] = "";

int main(int argc, char* argv[]) {
    WSADATA wsaData;
    SOCKET hSock;
    SOCKADDR_IN servAdr;
    HANDLE hRecvThread;
    char id[30], pw[30], servMsg[BUF_SIZE];
    int strLen;

    if (argc != 3) {
        printf("사용법: %s <서버 IP> <포트번호>\n", argv[0]);
        exit(1);
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) ErrorHandling("WSAStartup() error");

    hSock = socket(PF_INET, SOCK_STREAM, 0);

    memset(&servAdr, 0, sizeof(servAdr));
    servAdr.sin_family = AF_INET;
    servAdr.sin_addr.s_addr = inet_addr(argv[1]);
    servAdr.sin_port = htons(atoi(argv[2]));

    if (connect(hSock, (SOCKADDR*)&servAdr, sizeof(servAdr)) == SOCKET_ERROR)
        ErrorHandling("connect() 함수 오류");

    printf("ID: ");
    fgets(id, sizeof(id), stdin); id[strcspn(id, "\n")] = 0;
    printf("PW: ");
    fgets(pw, sizeof(pw), stdin); pw[strcspn(pw, "\n")] = 0;

    sprintf(msg, "%s\n%s\n##END##\n", id, pw);
    send(hSock, msg, strlen(msg), 0);

    strLen = recv(hSock, servMsg, BUF_SIZE - 1, 0);
    servMsg[strLen] = 0;

    if (strstr(servMsg, "LOGIN_SUCCESS")) {
        printf("로그인 성공\n");
        strcpy(name, id);
    } else {
        printf("로그인 실패\n");
        closesocket(hSock);
        return 1;
    }

    hRecvThread = (HANDLE)_beginthreadex(NULL, 0, RecvMsg, (void*)&hSock, 0, NULL);

    printf("참여 또는 생성할 프로젝트 이름 입력: ");
    fgets(currentProject, sizeof(currentProject), stdin);
    currentProject[strcspn(currentProject, "\n")] = 0;

    sprintf(msg, "create_project\n%s\n##END##\n", currentProject);
    send(hSock, msg, strlen(msg), 0);

    while (1) {
        printf("\n(Project: %s) 명령 입력 (commit, history, view, revoke, exit): ", currentProject);
        fgets(msg, BUF_SIZE, stdin);
        msg[strcspn(msg, "\n")] = 0;

        if (strcmp(msg, "exit") == 0) {
            break;
        } else if (strcmp(msg, "commit") == 0) {
            char title[100], commitMsg[512], code[8192] = "", temp[1024];
            printf("커밋 제목: ");
            fgets(title, sizeof(title), stdin); title[strcspn(title, "\n")] = 0;
            printf("커밋 메시지: ");
            fgets(commitMsg, sizeof(commitMsg), stdin); commitMsg[strcspn(commitMsg, "\n")] = 0;
            printf("코드 입력 (마지막 줄에 '##CODE_END##' 입력):\n");
            while (fgets(temp, sizeof(temp), stdin)) {
                if (strcmp(temp, "##CODE_END##\n") == 0) break;
                strcat(code, temp);
            }
            sprintf(msg, "commit\n%s\n%s\n%s\n%s##END##\n", currentProject, title, commitMsg, code);
        } else if (strcmp(msg, "history") == 0) {
            sprintf(msg, "history\n%s\n##END##\n", currentProject);
        } else if (strcmp(msg, "revoke") == 0) {
            char commitID[50];
            printf("취소할 커밋 ID: ");
            fgets(commitID, sizeof(commitID), stdin); commitID[strcspn(commitID, "\n")] = 0;
            sprintf(msg, "revoke\n%s\n%s\n##END##\n", currentProject, commitID);
        } else if (strcmp(msg, "view") == 0) {
            char commitID[50];
            printf("조회할 커밋 ID: ");
            fgets(commitID, sizeof(commitID), stdin); commitID[strcspn(commitID, "\n")] = 0;
            sprintf(msg, "view\n%s\n%s\n##END##\n", currentProject, commitID);
        } else {
            printf("잘못된 명령어입니다.\n");
            continue; // Don't send invalid commands
        }
        send(hSock, msg, strlen(msg), 0);
    }

    closesocket(hSock);
    WaitForSingleObject(hRecvThread, INFINITE);
    CloseHandle(hRecvThread);
    WSACleanup();
    return 0;
}

unsigned WINAPI RecvMsg(void* arg) {
    SOCKET hSock = *((SOCKET*)arg);
    char servMsg[MAX_MSG_LEN];
    int strLen;

    while ((strLen = recv(hSock, servMsg, MAX_MSG_LEN - 1, 0)) > 0) {
        servMsg[strLen] = 0;
        
        char* token_end = strstr(servMsg, "##END##");
        if(!token_end) continue;
        *token_end = '\0';

        if (strncmp(servMsg, "UPDATE", 6) == 0) {
            char* ptr = strtok(servMsg, "\n");
            ptr = strtok(NULL, "\n"); // project name
            if (ptr && strcmp(ptr, currentProject) == 0) {
                printf("\n[알림] 이 프로젝트에 변경사항이 발생했습니다.\n'history' 명령으로 로그를 새로고침하세요.\n");
            }
        } else if (strstr(servMsg, "PROJECT_CREATED")) {
            printf("[시스템] 프로젝트 '%s' 생성 완료.\n", currentProject);
        } else if (strstr(servMsg, "PROJECT_EXISTS")) {
            printf("[시스템] 기존 프로젝트 '%s'에 참여했습니다.\n", currentProject);
        } else if (strstr(servMsg, "COMMIT_SUCCESS")) {
            printf("[시스템] 커밋 성공.\n");
        } else if (strstr(servMsg, "REVOKE_SUCCESS")) {
            printf("[시스템] 커밋 취소 성공.\n");
        } else if (strstr(servMsg, "REVOKE_FAIL_NOT_FOUND")) {
            printf("[시스템] 취소할 커밋을 찾지 못했습니다.\n");
        } else if(strstr(servMsg, "HISTORY_CODE_RESPONSE")){
            char* code = strstr(servMsg, "\n") + 1;
            printf("\n--- 커밋 코드 상세 ---\n%s\n--- 코드 끝 ---\n", code);
        } else if(strstr(servMsg, "HISTORY_CODE_FAIL")){
            printf("[시스템] 해당 ID의 커밋 코드를 찾을 수 없습니다.\n");
        }
        else {
             DisplayHistory(servMsg);
        }
    }
    printf("서버 연결이 종료되었습니다.\n");
    return 0;
}

void DisplayHistory(char* historyData) {
    printf("\n--- %s 프로젝트 커밋 히스토리 ---\n", currentProject);

    if (strlen(historyData) == 0 || strstr(historyData, "히스토리가 없습니다")) {
        printf("%s\n", strlen(historyData) == 0 ? "이 프로젝트에 대한 히스토리가 없습니다." : historyData);
        printf("--- 히스토리 끝 ---\n");
        return;
    }

    CommitNode* head = NULL, *tail = NULL;
    char* context = NULL;
    char* line = strtok_s(historyData, "\n", &context);

    while (line != NULL) {
        CommitNode* newNode = (CommitNode*)malloc(sizeof(CommitNode));
        if (!newNode) { FreeHistory(head); return; }
        strncpy(newNode->data, line, sizeof(newNode->data) - 1);
        newNode->data[sizeof(newNode->data) - 1] = '\0';
        newNode->next = NULL;

        if (head == NULL) { head = tail = newNode; }
        else { tail->next = newNode; tail = newNode; }
        line = strtok_s(NULL, "\n", &context);
    }

    CommitNode* current = head;
    while (current != NULL) {
        printf("%s\n", current->data);
        current = current->next;
    }
    printf("--- 히스토리 끝 ---\n");
    FreeHistory(head);
}

void FreeHistory(CommitNode* head) {
    CommitNode* tmp;
    while (head != NULL) {
        tmp = head;
        head = head->next;
        free(tmp);
    }
}

void ErrorHandling(char* msg) {
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}