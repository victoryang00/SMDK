file -inlinebatch END_OF_BATCH

CREATE TABLE P1 (
    P1PK       BIGINT NOT NULL,
    INT1        INTEGER,
    VARCHAR1    VARCHAR(1000),
    PRIMARY KEY(P1PK));
PARTITION TABLE P1 ON COLUMN P1PK;

CREATE TABLE P2 (
    P2PK       BIGINT NOT NULL,
    P1FK       BIGINT NOT NULL,
    INT1        INTEGER,
    VARCHAR1    VARCHAR(1000),
    PRIMARY KEY(P2PK, P1FK));
PARTITION TABLE P2 ON COLUMN P1FK;

CREATE PROCEDURE InsertP1 PARTITION ON TABLE P1 COLUMN P1PK PARAMETER 0 AS
    Insert into P1 values (?, ?, ?);
CREATE PROCEDURE InsertP2 PARTITION ON TABLE P2 COLUMN P1FK PARAMETER 1 AS
    Insert into P2 values (?, ?, ?, ?);

END_OF_BATCH