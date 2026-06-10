CREATE VIEW  vw_Report_ActiveOrganisationSv2
AS
SELECT 
     * 
FROM
(
	SELECT	 
	
	OrgHeader.OH_Code As OrgCode,
	OrgHeader.OH_FullName As OrgName,
	RefUNLOCO.RL_PK as RefUNLOCOId,
	RefCountry.RN_PK As RefCountryId,
	RefCountry.RN_Code As RefCountryCode,
	RefUNLOCO.RL_Code As UNLOCO,
	OrgHeader.OH_Category As Category,
	OrgHeader.OH_ScreeningStatus As ScreeningStatus,
		(case when OrgHeader.OH_IsActive = 1 then
		'Active Accounts Only'
		else
		'Inactive Accounts Only'
		End) As Active,
	OB_GC as CompanyPK,	
	GB_PK AS OrgBranchPK,
	Branch=GB_Code,
    Company=GC_Code,

	OrgType=(CASE WHEN OH_IsConsignee           =1 THEN 'Consignee'
				  WHEN OH_IsConsignor           =1 THEN 'Consignor'
				  WHEN OH_IsForwarder           =1 THEN 'Forwarder'
				  WHEN OH_IsShippingProvider    =1 THEN 'Carrier'
			      WHEN OH_IsBroker              =1 THEN 'Broker'
				  WHEN OH_IsControllingAgent    =1 THEN 'ControllingAgent'
				  WHEN OH_IsControllingCustomer =1 THEN 'ControllingCustomer'
				  WHEN OH_IsGlobalAccount       =1 THEN 'GlobalAccount'
				  WHEN OH_IsLocalTransport      =1 THEN 'LocalTransport'
				  END),
	OrgAddress = OA_Address1 + ' ' + OA_Address2 + ' ' + OA_City + ' ' + OA_State + ' ' + OA_PostCode,
	OA_Address1,
	OA_Address2,
	OA_City,
	OA_State,
	OA_PostCode
			  
	FROM 
		   OrgHeader 
           OUTER APPLY dbo.MainAddressPkForOrg(OrgHeader.OH_PK) AS MainAddress 
           LEFT JOIN dbo.OrgAddress OrgHeaderMainAddress  on OrgHeaderMainAddress.OA_PK = MainAddress.PK 
		   JOIN dbo.OrgCompanyData  on OB_OH = OH_PK AND OB_IsValid=1
		   JOIN dbo.RefUNLOCO   on RefUNLOCO.RL_Code = OrgHeader.OH_RL_NKClosestPort 
		   JOIN dbo.RefCountry  On RefCountry.RN_Code = RefUNLOCO.RL_RN_NKCountryCode 
		   JOIN dbo.GlbBranch  on GB_PK = OB_GB_ControllingBranch
		   JOIN GlbCompany ON GC_PK=OB_GC
    WHERE OH_IsActive = 1

) InnerSel