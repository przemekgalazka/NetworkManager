/* NetworkManager -- Network link manager
 *
 * Tom Parker <palfrey@tevp.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * (C) Copyright 2004 Tom Parker
 */


#include "interface_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

if_block* first;
if_block* last;

if_data* last_data; 

void add_block(const char *type, const char* name)
{
	if_block *ret = (if_block*)calloc(1,sizeof(struct _if_block));
	ret->name = (char*)calloc(strlen(name),sizeof(char));
	strcpy(ret->name, name);
	ret->type = (char*)calloc(strlen(type),sizeof(char));
	strcpy(ret->type, type);
	if (first == NULL)
		first = last = ret;
	else
	{
		last->next = ret;
		last = ret;
	}
	last_data = NULL;
	//printf("added block '%s' with type '%s'\n",name,type);
}

void add_data(const char *key,const char *data)
{
	if_data *ret = (if_data*)calloc(1,sizeof(struct _if_data));
	ret->key = (char*)calloc(strlen(key),sizeof(char));
	strcpy(ret->key,key);
	ret->data = (char*)calloc(strlen(data),sizeof(char));
	strcpy(ret->data, data);
	
	if (last->info == NULL)
	{
		last->info = ret;
		last_data = ret;
	}
	else
	{
		last_data->next = ret;
		last_data = last_data->next;
	}
	//printf("added data '%s' with key '%s'\n",data,key);
}

void ifparser_init()
{
	FILE *inp = fopen(INTERFACES,"r");
	int ret = 0;
	first = last = NULL;
	while(1)
	{
		char *line,rline[255],*space;
		if (ret == EOF)
			break;
		ret = fscanf(inp,"%255[^\n]\n",rline);
		line = rline;
		while(line[0] == ' ')
			line++;
		if (line[0]=='#' || line[0]=='\0')
			continue;
		
		space = strchr(line,' ');
		if (space == NULL)
		{
			fprintf(stderr,"Can't parse line '%s'\n",line);
			continue;
		}
		space[0] = '\0';
		
		
		if (strcmp(line,"iface")==0)
		{
			char *space2 = strchr(space+1,' ');
			if (space2 == NULL)
			{
				fprintf(stderr,"Can't parse iface line '%s'\n",space+1);
				continue;
			}
			space2[0]='\0';
			add_block(line,space+1);

			if (space2[1]!='\0')
			{
				space = strchr(space2+1,' ');
				if (space == NULL)
				{
					fprintf(stderr,"Can't parse data '%s'\n",space2+1);
					continue;
				}
				space[0] = '\0';
				add_data(space2+1,space+1);
			}
		}
		else if (strcmp(line,"auto")==0)
			add_block(line,space+1);
		else
			add_data(line,space+1);
		
		//printf("line: '%s' ret=%d\n",rline,ret);
	}
	fclose(inp);
}	
	
void _destroy_data(if_data *ifd)
{
	if (ifd == NULL)
		return;
	_destroy_data(ifd->next);
	free(ifd->key);
	free(ifd->data);
	free(ifd);
	return;
}

void _destroy_block(if_block* ifb)
{
	if (ifb == NULL)
		return;
	_destroy_block(ifb->next);
	_destroy_data(ifb->info);
	free(ifb->name);
	free(ifb->type);
	free(ifb);
	return;
}

void ifparser_destroy() 
{
	_destroy_block(first);
	first = last = NULL;
}

if_block *ifparser_getif(const char* iface)
{
	if_block *curr = first;
	while(curr!=NULL)
	{
		if (strcmp(curr->type,"iface")==0 && strcmp(curr->name,iface)==0)
			return curr;
		curr = curr->next;
	}
	return NULL;
}

const char *ifparser_getkey(if_block* iface, const char *key)
{
	if_data *curr = iface->info;
	while(curr!=NULL)
	{
		if (strcmp(curr->key,key)==0)
			return curr->data;
		curr = curr->next;
	}
	return NULL;
}
