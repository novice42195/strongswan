/*
 * Copyright (C) 2008-2009 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "eap_aka_3gpp2_card.h"

#include <daemon.h>

typedef struct private_eap_aka_3gpp2_card_t private_eap_aka_3gpp2_card_t;

/**
 * Private data of an eap_aka_3gpp2_card_t object.
 */
struct private_eap_aka_3gpp2_card_t {

	/**
	 * Public eap_aka_3gpp2_card_t interface.
	 */
	eap_aka_3gpp2_card_t public;

	/**
	 * AKA functions
	 */
	eap_aka_3gpp2_functions_t *f;

	/**
	 * do sequence number checking?
	 */
	bool seq_check;

	/**
	 * SQN stored in this pseudo-USIM
	 */
	char sqn[AKA_SQN_LEN];
};

/**
 * Functions from eap_aka_3gpp2_provider.c
 */
bool eap_aka_3gpp2_get_k(identification_t *id, char k[AKA_K_LEN]);
void eap_aka_3gpp2_get_sqn(char sqn[AKA_SQN_LEN], int offset);

/**
 * Implementation of usim_card_t.get_quintuplet
 */
static status_t get_quintuplet(private_eap_aka_3gpp2_card_t *this,
							   identification_t *imsi, char rand[AKA_RAND_LEN],
							   char autn[AKA_AUTN_LEN], char ck[AKA_CK_LEN],
							   char ik[AKA_IK_LEN], char res[AKA_RES_LEN])
{
	char *amf, *mac;
	char k[AKA_K_LEN], ak[AKA_AK_LEN], sqn[AKA_SQN_LEN], xmac[AKA_MAC_LEN];

	if (!eap_aka_3gpp2_get_k(imsi, k))
	{
		DBG1(DBG_IKE, "no EAP key found for %Y to authenticate with AKA", imsi);
		return FALSE;
	}

	/* AUTN = SQN xor AK | AMF | MAC */
	DBG3(DBG_IKE, "received autn %b", autn, sizeof(autn));
	DBG3(DBG_IKE, "using K %b", k, sizeof(k));
	DBG3(DBG_IKE, "using rand %b", rand, sizeof(rand));
	memcpy(sqn, autn, sizeof(sqn));
	amf = autn + sizeof(sqn);
	mac = autn + sizeof(sqn) + AKA_AMF_LEN;

	/* XOR anonymity key AK into SQN to decrypt it */
	this->f->f5(this->f, k, rand, ak);
	DBG3(DBG_IKE, "using ak %b", ak, sizeof(ak));
	memxor(sqn, ak, sizeof(sqn));
	DBG3(DBG_IKE, "using sqn %b", sqn, sizeof(sqn));

	/* calculate expected MAC and compare against received one */
	this->f->f1(this->f, k, rand, sqn, amf, xmac);
	if (!memeq(mac, xmac, sizeof(xmac)))
	{
		DBG1(DBG_IKE, "received MAC does not match XMAC");
		DBG3(DBG_IKE, "MAC %b\nXMAC %b", mac, AKA_MAC_LEN, xmac, AKA_MAC_LEN);
		return FAILED;
	}

	if (this->seq_check && memcmp(this->sqn, sqn, sizeof(sqn)) >= 0)
	{
		DBG3(DBG_IKE, "received SQN %b\ncurrent SQN %b",
			 sqn, sizeof(sqn), this->sqn, sizeof(this->sqn));
		return INVALID_STATE;
	}

	/* update stored SQN to the received one */
	memcpy(this->sqn, sqn, sizeof(sqn));

	/* calculate RES */
	this->f->f2(this->f, k, rand, res);
	DBG3(DBG_IKE, "calculated rand %b", res, sizeof(res));

	return SUCCESS;
}

/**
 * Implementation of usim_card_t.resync
 */
static bool resync(private_eap_aka_3gpp2_card_t *this, identification_t *imsi,
				   char rand[AKA_RAND_LEN], char auts[AKA_AUTS_LEN])
{
	char amf[AKA_AMF_LEN], k[AKA_K_LEN], aks[AKA_AK_LEN], macs[AKA_MAC_LEN];

	if (!eap_aka_3gpp2_get_k(imsi, k))
	{
		DBG1(DBG_IKE, "no EAP key found for %Y to resync AKA", imsi);
		return FALSE;
	}

	/* AMF is set to zero in resync */
	memset(amf, 0, sizeof(amf));
	this->f->f5star(this->f, k, rand, aks);
	this->f->f1star(this->f, k, rand, this->sqn, amf, macs);
	/* AUTS = SQN xor AKS | MACS */
	memcpy(auts, this->sqn, sizeof(this->sqn));
	memxor(auts, aks, sizeof(aks));
	memcpy(auts + sizeof(aks), macs, sizeof(macs));

	return TRUE;
}

/**
 * Implementation of eap_aka_3gpp2_card_t.destroy.
 */
static void destroy(private_eap_aka_3gpp2_card_t *this)
{
	free(this);
}

/**
 * See header
 */
eap_aka_3gpp2_card_t *eap_aka_3gpp2_card_create(eap_aka_3gpp2_functions_t *f)
{
	private_eap_aka_3gpp2_card_t *this = malloc_thing(private_eap_aka_3gpp2_card_t);

	this->public.card.get_quintuplet = (status_t(*)(usim_card_t*,  identification_t *imsi, char rand[16], char autn[16], char ck[16], char ik[16], char res[16]))get_quintuplet;
	this->public.card.resync = (bool(*)(usim_card_t*, identification_t *imsi, char rand[16], char auts[14]))resync;
	this->public.destroy = (void(*)(eap_aka_3gpp2_card_t*))destroy;

	this->f = f;
	this->seq_check = lib->settings->get_bool(lib->settings,
									"charon.plugins.eap_aka_3gpp2.seq_check",
#ifdef SEQ_CHECK /* handle legacy compile time configuration as default */
									TRUE);
#else /* !SEQ_CHECK */
									FALSE);
#endif /* SEQ_CHECK */

	eap_aka_3gpp2_get_sqn(this->sqn, 0);

	return &this->public;
}

