CREATE DATABASE nvme_discdb CHARACTER SET ascii;

USE nvme_discdb;

CREATE TABLE host (
    id INT NOT NULL AUTO_INCREMENT,
    PRIMARY KEY (id),
    nqn CHAR(223) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
);

CREATE TABLE subsys (
    id INT NOT NULL AUTO_INCREMENT,
    PRIMARY KEY (id),
    nqn CHAR(223) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
);

CREATE TABLE port (
    portid INT NOT NULL,
    PRIMARY KEY (portid),
    trtype INT DEFAULT 3,
    adrfam INT DEFAULT 1,
    subtype INT DEFAULT 2,
    treq INT DEFAULT 0,
    traddr CHAR(255) NOT NULL,
    trsvcid CHAR(32) DEFAULT '',
    tsas CHAR(255) DEFAULT ''
);
    
CREATE TABLE host_subsys (
    id INT NOT NULL AUTO_INCREMENT,
    host_id INT NOT NULL,
    subsys_id INT NOT NULL,

    PRIMARY KEY (id),
    INDEX host (host_id),
    INDEX subsys (subsys_id),

    FOREIGN KEY (host_id)
      REFERENCES host(id)
      ON UPDATE CASCADE ON DELETE RESTRICT,
    FOREIGN KEY (subsys_id)
      REFERENCES subsys(id)
      ON UPDATE CASCADE ON DELETE RESTRICT
);

CREATE TABLE subsys_port (
    id INT NOT NULL AUTO_INCREMENT,
    subsys_id INT NOT NULL,
    port_id INT NOT NULL,

    PRIMARY KEY (id),
    INDEX subsys (subsys_id),
    INDEX port (port_id),

    FOREIGN KEY (subsys_id)
      REFERENCES subsys(id)
      ON UPDATE CASCADE ON DELETE RESTRICT,
    FOREIGN KEY (port_id)
      REFERENCES port(portid)
      ON UPDATE CASCADE ON DELETE RESTRICT
);

INSERT INTO host (nqn) VALUES('nqn.blktests-hosts-1');
INSERT INTO subsys (nqn) VALUES('nqn.blktests-subsys-1');
INSERT INTO port (portid, traddr, trsvcid)
  VALUES (1, '127.0.0.1', 8009), (2, '127.0.0.1', 8010);
INSERT INTO host_subsys (host_id, subsys_id)
  SELECT host.id, subsys.id FROM host, subsys
  WHERE host.nqn LIKE 'nqn.blktests-host-1' AND
    subsys.nqn LIKE 'nqn.blktests-subsys-1';
INSERT INTO port_subsys (subsys_id, port_id)
  SELECT subsys.id, port.portid FROM subsys, port
    WHERE subsys.nqn LIKE 'nqn.blktests-subsys-1' AND port.portid = 1;

SELECT subsys.nqn, port.portid FROM subsys, port, subsys_port
  WHERE subsys.id = subsys_port.subsys_id AND
        port.portid = subsys_port.port_id;

