-- $Id$d$
--
-- schema.sql   rlm_sql - FreeRADIUS SQL Module
--
-- Database schema for MSSQL rlm_sql module
--
-- To load:
--  isql -S db_ip_addr -d db_name -U db_login -P db_passwd -i db_mssql.sql
--
-- Based on: db_mysql.sql (Mike Machado <mike@innercite.com>)
--
--					Dmitri Ageev <d_ageev@ortcc.ru>
--


--
-- Table structure for table 'radacct'
--

CREATE TABLE [radacct] (
	[RadAcctId] [numeric](21, 0) IDENTITY (1, 1) NOT NULL,
	[AcctSessionId] [varchar] (64) NOT NULL,
	[AcctUniqueId] [varchar] (32) NOT NULL,
	[UserName] [varchar] (64) NOT NULL,
	[GroupName] [varchar] (64) NOT NULL,
	[Realm] [varchar] (64) NOT NULL,
	[NASIPAddress] [varchar] (15) NOT NULL,
	[NASPortId] [varchar] (15) NULL,
	[NASPortType] [varchar] (32) NULL,
	[AcctStartTime] [datetime] NOT NULL,
	[AcctUpdateTime] [datetime] NOT NULL,
	[AcctStopTime] [datetime] NOT NULL,
	[AcctInterval] [bigint] NULL,
	[AcctSessionTime] [bigint] NULL,
	[AcctAuthentic] [varchar] (32) NULL,
	[ConnectInfo_start] [varchar] (32) NULL,
	[ConnectInfo_stop] [varchar] (32) NULL,
	[AcctInputOctets] [bigint] NULL,
	[AcctOutputOctets] [bigint] NULL,
	[CalledStationId] [varchar] (30) NOT NULL,
	[CallingStationId] [varchar] (30) NOT NULL,
	[AcctTerminateCause] [varchar] (32) NOT NULL,
	[ServiceType] [varchar] (32) NULL,
	[FramedProtocol] [varchar] (32) NULL,
	[FramedIPAddress] [varchar] (15) NOT NULL,
	[FramedIPv6Address] [varchar] (45) NOT NULL,
	[FramedIPv6Prefix] [varchar] (45) NOT NULL,
	[FramedInterfaceId] [varchar] (44) NOT NULL,
	[DelegatedIPv6Prefix] [varchar] (45) NOT NULL,
	[Class] [varchar] (64) NULL
) ON [PRIMARY]
GO

ALTER TABLE [radacct] WITH NOCHECK ADD
	CONSTRAINT [DF_radacct_GroupName] DEFAULT ('') FOR [GroupName],
	CONSTRAINT [DF_radacct_AcctSessionId] DEFAULT ('') FOR [AcctSessionId],
	CONSTRAINT [DF_radacct_AcctUniqueId] DEFAULT ('') FOR [AcctUniqueId],
	CONSTRAINT [DF_radacct_UserName] DEFAULT ('') FOR [UserName],
	CONSTRAINT [DF_radacct_Realm] DEFAULT ('') FOR [Realm],
	CONSTRAINT [DF_radacct_NASIPAddress] DEFAULT ('') FOR [NASIPAddress],
	CONSTRAINT [DF_radacct_NASPortId] DEFAULT (null) FOR [NASPortId],
	CONSTRAINT [DF_radacct_NASPortType] DEFAULT (null) FOR [NASPortType],
	CONSTRAINT [DF_radacct_AcctStartTime] DEFAULT ('1900-01-01 00:00:00') FOR [AcctStartTime],
	CONSTRAINT [DF_radacct_AcctUpdateTime] DEFAULT ('1900-01-01 00:00:00') FOR [AcctUpdateTime],
	CONSTRAINT [DF_radacct_AcctStopTime] DEFAULT ('1900-01-01 00:00:00') FOR [AcctStopTime],
	CONSTRAINT [DF_radacct_AcctSessionTime] DEFAULT (null) FOR [AcctSessionTime],
	CONSTRAINT [DF_radacct_AcctAuthentic] DEFAULT (null) FOR [AcctAuthentic],
	CONSTRAINT [DF_radacct_ConnectInfo_start] DEFAULT (null) FOR [ConnectInfo_start],
	CONSTRAINT [DF_radacct_ConnectInfo_stop] DEFAULT (null) FOR [ConnectInfo_stop],
	CONSTRAINT [DF_radacct_AcctInputOctets] DEFAULT (null) FOR [AcctInputOctets],
	CONSTRAINT [DF_radacct_AcctOutputOctets] DEFAULT (null) FOR [AcctOutputOctets],
	CONSTRAINT [DF_radacct_CalledStationId] DEFAULT ('') FOR [CalledStationId],
	CONSTRAINT [DF_radacct_CallingStationId] DEFAULT ('') FOR [CallingStationId],
	CONSTRAINT [DF_radacct_AcctTerminateCause] DEFAULT ('') FOR [AcctTerminateCause],
	CONSTRAINT [DF_radacct_ServiceType] DEFAULT (null) FOR [ServiceType],
	CONSTRAINT [DF_radacct_FramedProtocol] DEFAULT (null) FOR [FramedProtocol],
	CONSTRAINT [DF_radacct_FramedIPAddress] DEFAULT ('') FOR [FramedIPAddress],
	CONSTRAINT [DF_radacct_FramedIPv6Address] DEFAULT ('') FOR [FramedIPv6Address],
	CONSTRAINT [DF_radacct_FramedIPv6Prefix] DEFAULT ('') FOR [FramedIPv6Prefix],
	CONSTRAINT [DF_radacct_FramedInterfaceId] DEFAULT ('') FOR [FramedInterfaceId],
	CONSTRAINT [DF_radacct_DelegatedIPv6Prefix] DEFAULT ('') FOR [DelegatedIPv6Prefix],
	CONSTRAINT [DF_radacct_Class] DEFAULT (null) FOR [Class],
	CONSTRAINT [PK_radacct] PRIMARY KEY NONCLUSTERED
	(
		[RadAcctId]
	) ON [PRIMARY]
GO

CREATE INDEX [UserName] ON [radacct]([UserName]) ON [PRIMARY]
GO

CREATE INDEX [FramedIPAddress] ON [radacct]([FramedIPAddress]) ON [PRIMARY]
GO

CREATE INDEX [FramedIPv6Address] ON [radacct]([FramedIPv6Address]) ON [PRIMARY]
GO

CREATE INDEX [FramedIPv6Prefix] ON [radacct]([FramedIPv6Prefix]) ON [PRIMARY]
GO

CREATE INDEX [FramedInterfaceId] ON [radacct]([FramedInterfaceId]) ON [PRIMARY]
GO

CREATE INDEX [DelegatedIPv6Prefix] ON [radacct]([DelegatedIPv6Prefix]) ON [PRIMARY]
GO

CREATE INDEX [AcctSessionId] ON [radacct]([AcctSessionId]) ON [PRIMARY]
GO

CREATE UNIQUE INDEX [AcctUniqueId] ON [radacct]([AcctUniqueId]) ON [PRIMARY]
GO

CREATE INDEX [AcctStartTime] ON [radacct]([AcctStartTime]) ON [PRIMARY]
GO

CREATE INDEX [AcctStopTime] ON [radacct]([AcctStopTime]) ON [PRIMARY]
GO

CREATE INDEX [NASIPAddress] ON [radacct]([NASIPAddress]) ON [PRIMARY]
GO

CREATE INDEX [Class] ON [radacct]([Class]) ON [PRIMARY]
GO

-- For use by onoff
CREATE INDEX [RadacctBulkClose] ON [radacct]([NASIPAddress],[AcctStartTime]) WHERE [AcctStopTime] IS NULL ON [PRIMARY]
GO


--
-- Table structure for table 'radcheck'
--
-- Note: [op] is varchar to allow for "=" as a value -
-- depending on which driver is used to access the database, if
-- the field is defined as char, then the trailing space may be
-- returned, which fails to parse correctly.
--

CREATE TABLE [radcheck] (
	[id] [int] IDENTITY (1, 1) NOT NULL ,
	[UserName] [varchar] (64) NOT NULL ,
	[Attribute] [varchar] (32) NOT NULL ,
	[Value] [varchar] (253) NOT NULL ,
	[op] [varchar] (2) NULL
) ON [PRIMARY]
GO

ALTER TABLE [radcheck] WITH NOCHECK ADD
	CONSTRAINT [DF_radcheck_UserName] DEFAULT ('') FOR [UserName],
	CONSTRAINT [DF_radcheck_Attribute] DEFAULT ('') FOR [Attribute],
	CONSTRAINT [DF_radcheck_Value] DEFAULT ('') FOR [Value],
	CONSTRAINT [DF_radcheck_op] DEFAULT (null) FOR [op],
	CONSTRAINT [PK_radcheck] PRIMARY KEY NONCLUSTERED
	(
		[id]
	) ON [PRIMARY]
GO

CREATE INDEX [UserName] ON [radcheck]([UserName]) ON [PRIMARY]
GO


--
-- Table structure for table 'radgroupcheck'
--

CREATE TABLE [radgroupcheck] (
	[id] [int] IDENTITY (1, 1) NOT NULL ,
	[GroupName] [varchar] (64) NOT NULL ,
	[Attribute] [varchar] (32) NOT NULL ,
	[Value] [varchar] (253) NOT NULL ,
	[op] [varchar] (2) NULL
) ON [PRIMARY]
GO

ALTER TABLE [radgroupcheck] WITH NOCHECK ADD
	CONSTRAINT [DF_radgroupcheck_GroupName] DEFAULT ('') FOR [GroupName],
	CONSTRAINT [DF_radgroupcheck_Attribute] DEFAULT ('') FOR [Attribute],
	CONSTRAINT [DF_radgroupcheck_Value] DEFAULT ('') FOR [Value],
	CONSTRAINT [DF_radgroupcheck_op] DEFAULT (null) FOR [op],
	CONSTRAINT [PK_radgroupcheck] PRIMARY KEY NONCLUSTERED
	(
		[id]
	) ON [PRIMARY]
GO

CREATE INDEX [GroupName] ON [radgroupcheck]([GroupName]) ON [PRIMARY]
GO


--
-- Table structure for table 'radgroupreply'
--

CREATE TABLE [radgroupreply] (
	[id] [int] IDENTITY (1, 1) NOT NULL ,
	[GroupName] [varchar] (64) NOT NULL ,
	[Attribute] [varchar] (32) NOT NULL ,
	[Value] [varchar] (253) NOT NULL ,
	[op] [varchar] (2) NULL ,
	[prio] [int] NOT NULL
) ON [PRIMARY]
GO

ALTER TABLE [radgroupreply] WITH NOCHECK ADD
	CONSTRAINT [DF_radgroupreply_GroupName] DEFAULT ('') FOR [GroupName],
	CONSTRAINT [DF_radgroupreply_Attribute] DEFAULT ('') FOR [Attribute],
	CONSTRAINT [DF_radgroupreply_Value] DEFAULT ('') FOR [Value],
	CONSTRAINT [DF_radgroupreply_op] DEFAULT (null) FOR [op],
	CONSTRAINT [DF_radgroupreply_prio] DEFAULT (0) FOR [prio],
	CONSTRAINT [PK_radgroupreply] PRIMARY KEY NONCLUSTERED
	(
		[id]
	) ON [PRIMARY]
GO

CREATE INDEX [GroupName] ON [radgroupreply]([GroupName]) ON [PRIMARY]
GO


--
-- Table structure for table 'radreply'
--

CREATE TABLE [radreply] (
	[id] [int] IDENTITY (1, 1) NOT NULL ,
	[UserName] [varchar] (64) NOT NULL ,
	[Attribute] [varchar] (32) NOT NULL ,
	[Value] [varchar] (253) NOT NULL ,
	[op] [varchar] (2) NULL
) ON [PRIMARY]
GO

ALTER TABLE [radreply] WITH NOCHECK ADD
	CONSTRAINT [DF_radreply_UserName] DEFAULT ('') FOR [UserName],
	CONSTRAINT [DF_radreply_Attribute] DEFAULT ('') FOR [Attribute],
	CONSTRAINT [DF_radreply_Value] DEFAULT ('') FOR [Value],
	CONSTRAINT [DF_radreply_op] DEFAULT (null) FOR [op],
	CONSTRAINT [PK_radreply] PRIMARY KEY NONCLUSTERED
	(
		[id]
	) ON [PRIMARY]
GO

CREATE INDEX [UserName] ON [radreply]([UserName]) ON [PRIMARY]
GO


--
-- Table structure for table 'radusergroup'
--

CREATE TABLE [radusergroup] (
	[id] [int] IDENTITY (1, 1) NOT NULL ,
	[UserName] [varchar] (64) NOT NULL ,
	[GroupName] [varchar] (64) NULL ,
	[Priority] [int] NULL
) ON [PRIMARY]
GO

ALTER TABLE [radusergroup] WITH NOCHECK ADD
	CONSTRAINT [DF_radusergroup_UserName] DEFAULT ('') FOR [UserName],
	CONSTRAINT [DF_radusergroup_GroupName] DEFAULT ('') FOR [GroupName],
	CONSTRAINT [PK_radusergroup] PRIMARY KEY NONCLUSTERED
	(
		[id]
	) ON [PRIMARY]
GO

CREATE INDEX [UserName] ON [radusergroup]([UserName]) ON [PRIMARY]
GO


--
-- Table structure for table 'radpostauth'
--

CREATE TABLE [radpostauth] (
	[id] [int] IDENTITY (1, 1) NOT NULL ,
	[userName] [varchar] (64) NOT NULL ,
	[pass] [varchar] (64) NOT NULL ,
	[reply] [varchar] (32) NOT NULL ,
	[authdate] [datetime] NOT NULL,
	[class] [varchar] (64) NULL
)
GO

ALTER TABLE [radpostauth] WITH NOCHECK ADD
	CONSTRAINT [DF_radpostauth_userName] DEFAULT ('') FOR [userName],
	CONSTRAINT [DF_radpostauth_pass] DEFAULT ('') FOR [pass],
	CONSTRAINT [DF_radpostauth_reply] DEFAULT ('') FOR [reply],
	CONSTRAINT [DF_radpostauth_authdate] DEFAULT (getdate()) FOR [authdate],
	CONSTRAINT [DF_radpostauth_class] DEFAULT ('') FOR [class],
	CONSTRAINT [PK_radpostauth] PRIMARY KEY NONCLUSTERED
	(
		[id]
	) ON [PRIMARY]
GO

--
-- Table structure for table 'nas'
--
CREATE TABLE [nas] (
	[id] [int] IDENTITY (1, 1) NOT NULL ,
	[nasname] [varchar] (128) NOT NULL,
	[shortname] [varchar] (32) NOT NULL,
	[type] [varchar] (30) NOT NULL,
	[ports] [int] NULL,
	[secret] [varchar] (60) NOT NULL,
	[server] [varchar] (64) NULL,
	[community] [varchar] (50) NULL,
	[description] [varchar] (200) NOT NULL,
	[require_ma] [varchar] (4) NOT NULL,
	[limit_proxy_state] [varchar] (4) NOT NULL
) ON [PRIMARY]
GO

CREATE INDEX [nas_name] ON [nas]([nasname]) ON [PRIMARY]
GO

ALTER TABLE [nas] WITH NOCHECK ADD
	CONSTRAINT [DF_nas_type] DEFAULT ('other') FOR [type],
	CONSTRAINT [DF_nas_secret] DEFAULT ('secret') FOR [secret],
	CONSTRAINT [DF_nas_description] DEFAULT ('RADIUS Client') FOR [description],
	CONSTRAINT [DF_require_ma] DEFAULT ('auto') FOR [require_ma],
	CONSTRAINT [DF_limit_proxy_state] DEFAULT ('auto') FOR [limit_proxy_state]
GO

--
-- Table structure for table 'nasreload'
--
CREATE TABLE [nasreload] (
	[nasipaddress] [varchar] (15) NOT NULL PRIMARY KEY,
	[reloadtime] [datetime] NOT NULL
) ON [PRIMARY]
GO
