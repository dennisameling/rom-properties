/***************************************************************************
 * ROM Properties Page shell extension. (rpcli)                            *
 * properties.cpp: Properties output.                                      *
 *                                                                         *
 * Copyright (c) 2016 by Egor.                                             *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU General Public License as published by the   *
 * Free Software Foundation; either version 2 of the License, or (at your  *
 * option) any later version.                                              *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.           *
 ***************************************************************************/
#include "properties.hpp"
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <libromdata/RomData.hpp>
#include <libromdata/RomFields.hpp>
#include <libromdata/TextFuncs.hpp>
using std::setw;
using std::left;
using std::ostream;
using std::max;
using std::endl;
using namespace LibRomData;
class Pad{
	size_t width;
public:
	Pad(size_t width):width(width){}
	friend ostream& operator<<(ostream& os,const Pad& pad){
		return os << setw(pad.width) << "";
	}
	
};
class ColonPad{
	size_t width;
	const rp_char* str;
public:
	ColonPad(size_t width,const rp_char* str):width(width),str(str){}
	friend ostream& operator<<(ostream& os,const ColonPad& cp){
		return os << cp.str << left << setw(max(0,(signed)( cp.width - rp_strlen(cp.str) ))) << ":";
	}
};
class StringField{
	size_t width;
	const RomFields::Desc* desc;
	const RomFields::Data* data;
public:
	StringField(size_t width, const RomFields::Desc* desc, const RomFields::Data* data):width(width),desc(desc),data(data){}
	friend ostream& operator<<(ostream& os,const StringField& field){
		auto desc = field.desc;
		auto data = field.data;
		return os << ColonPad(field.width,desc->name) << "'" << data->str << "'";
	}
};

class BitfieldField{
	size_t width;
	const RomFields::Desc* desc;
	const RomFields::Data* data;
public:
	BitfieldField(size_t width, const RomFields::Desc* desc, const RomFields::Data* data):width(width),desc(desc),data(data){}
	friend ostream& operator<<(ostream& os,const BitfieldField& field){
		auto desc = field.desc;
		auto data = field.data;
		
		int perRow = desc->bitfield->elemsPerRow ? desc->bitfield->elemsPerRow : 4;
		
		size_t *colSize = new size_t[perRow]();
		for(int i=0;i<desc->bitfield->elements;i++){
			colSize[i%perRow] = max(rp_strlen(desc->bitfield->names[i]),colSize[i%perRow]);
		}
		
		os << ColonPad(field.width,desc->name); // ColonPad sets std::left
		
		for(int i=0;i<desc->bitfield->elements;i++){
			if(i && i%perRow == 0) os << endl << Pad(field.width);
			os << " [" << (data->bitfield & (1<<i) ? '*' : ' ') << "] " <<
				setw(colSize[i%perRow]) << desc->bitfield->names[i];
		}
		delete[] colSize;
		return os;
	}
};

class ListDataField{
	size_t width;
	const RomFields::Desc* desc;
	const RomFields::Data* data;
public:
	ListDataField(size_t width, const RomFields::Desc* desc, const RomFields::Data* data):width(width),desc(desc),data(data){}
	friend ostream& operator<<(ostream& os,const ListDataField& field){
		auto desc = field.desc;
		auto data = field.data;
		
		size_t *colSize = new size_t[desc->list_data->count]();
		size_t totalWidth = desc->list_data->count + 1;
		for(int i=0;i<desc->list_data->count;i++){
			colSize[i] = rp_strlen(desc->list_data->names[i]); 
		}
		for(auto it = data->list_data->data.begin(); it != data->list_data->data.end(); ++it){
			int i=0;
			for(auto jt = it->begin(); jt != it->end(); ++jt){
				colSize[i] = max(jt->length(),colSize[i]);
				i++;
			}
		}
		
		os << ColonPad(field.width,desc->name); // ColonPad sets std::left
		
		for(int i=0;i<desc->list_data->count;i++){
			totalWidth += colSize[i]; // this could be in a separate loop, but whatever
			os << "|" << setw(colSize[i]) << desc->list_data->names[i];
		}
		os << "|" << endl << Pad(field.width) << rp_string(totalWidth,'-');
		for(auto it = data->list_data->data.begin(); it != data->list_data->data.end(); ++it){
			int i=0;
			os << endl << Pad(field.width);
			for(auto jt = it->begin(); jt != it->end(); ++jt){
				os << "|" << setw(colSize[i++]) << *jt;
			}
			os << "|";
		}
		delete[] colSize;
		return os;
	}
};
ostream& operator<<(ostream& os,const RomFields& fields){
	size_t maxWidth=0;
	for(int i=0;i<fields.count();i++){
		maxWidth=max(maxWidth,rp_strlen(fields.desc(i)->name));
	}
	maxWidth+=2;
	for(int i=0;i<fields.count();i++){
		auto desc = fields.desc(i);
		auto data = fields.data(i);
		if(i) os << endl;
		switch(desc->type){
			case RomFields::RFT_STRING:{
				os << StringField(maxWidth,desc,data);
				break;
			}
			case RomFields::RomFields::RFT_BITFIELD:{
				os << BitfieldField(maxWidth,desc,data);
				break;
			}
			case RomFields::RomFields::RFT_LISTDATA:{
				os << ListDataField(maxWidth,desc,data);
				break;
			}
			default:{
				os << ColonPad(maxWidth,desc->name) << "NYI";
			}
		}
	}
	return os;
}
